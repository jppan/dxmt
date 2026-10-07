#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "dxgi_options.hpp"
#include "util_string.hpp"
#include "thread.hpp"
#include "log/log.hpp"
#include "wsi_monitor.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "d3d10_1.h"
#include "Metal.hpp"
#include <atomic>
#include <cstdio>     /* ml1007 */
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dxmt {

Com<IDXGIOutput> CreateOutput(IMTLDXGIAdapter *pAadapter, HMONITOR monitor, DxgiOptions &options);

LUID GetAdapterLuid(WMT::Device device) {
    // NOTE: use big-endian registryID, be consistent with MVK
  return std::bit_cast<LUID>(__builtin_bswap64(device.registryID()));
}

class MTLDXGIAdatper : public MTLDXGIObject<IMTLDXGIAdapter> {
public:
  MTLDXGIAdatper(WMT::Device device, IDXGIFactory *factory, Config &config)
      : device_(device), factory_(factory), options_(config) {
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid = GetAdapterLuid(device_);
    if (D3DKMTOpenAdapterFromLuid(&open))
      WARN("Failed to open D3DKMT adapter");
    else
      local_kmt_ = open.hAdapter;
  };

  ~MTLDXGIAdatper() {
    /* ml2052: stop the budget poller before device_ and its event table are
     * destroyed.  Its 250 ms polling period also bounds this join. */
    budget_notify_stop_.store(true, std::memory_order_release);
    if (budget_notify_thread_.joinable())
      budget_notify_thread_.join();
    if (local_kmt_) {
      D3DKMT_CLOSEADAPTER close = {};
      close.hAdapter = local_kmt_;
      if (D3DKMTCloseAdapter(&close))
        WARN("Failed to close D3DKMT adapter");
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIAdapter) || riid == __uuidof(IDXGIAdapter1) ||
        riid == __uuidof(IDXGIAdapter2) || riid == __uuidof(IDXGIAdapter3) ||
        riid == __uuidof(IDXGIAdapter4) || riid == __uuidof(IMTLDXGIAdapter)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIAdapter), riid)) {
      WARN("DXGIAdapter: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    return factory_->QueryInterface(riid, ppParent);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
    }

    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
      pDesc->Flags = desc.Flags & 0b11;
    }

    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2 *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
      pDesc->Flags = desc.Flags & 0b11;
      pDesc->GraphicsPreemptionGranularity = desc.GraphicsPreemptionGranularity;
      pDesc->ComputePreemptionGranularity = desc.ComputePreemptionGranularity;
    }

    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3 *pDesc) final {
    if (pDesc == nullptr)
      return E_INVALIDARG;

    std::memset(pDesc->Description, 0, sizeof(pDesc->Description));

    if (!options_.customDeviceDesc.empty()) {
      str::transcodeString(
          pDesc->Description,
          sizeof(pDesc->Description) / sizeof(pDesc->Description[0]) - 1,
          options_.customDeviceDesc.c_str(), options_.customDeviceDesc.size());
    } else {
      device_.name().getCString((char *)pDesc->Description, sizeof(pDesc->Description), WMTUTF16StringEncoding);
    }

    if (options_.customVendorId >= 0) {
      pDesc->VendorId = options_.customVendorId;
    } else {
      pDesc->VendorId = 0x106B;
      if (g_extension_enabled == VendorExtension::Nvidia) {
        pDesc->VendorId = 0x10DE;
      }
    }

    if (options_.customDeviceId >= 0) {
      pDesc->DeviceId = options_.customDeviceId;
    } else {
      pDesc->DeviceId = 0;
    }

    pDesc->SubSysId = 0;
    pDesc->Revision = 0;
    // ml1042: the unix side now returns an honest per-process video budget (see
    // winemetal_unix.c), so report it whole. The old "/2 on unified memory" was a
    // guess compensating for a number that was far too large; halving a correct
    // number would just starve the application of half its real budget.
    pDesc->DedicatedVideoMemory = device_.recommendedMaxWorkingSetSize();
    pDesc->DedicatedSystemMemory = 0;
    pDesc->SharedSystemMemory = 0;
    pDesc->AdapterLuid = GetAdapterLuid(device_);
    pDesc->Flags = DXGI_ADAPTER_FLAG3_NONE;
    pDesc->GraphicsPreemptionGranularity = DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
    pDesc->ComputePreemptionGranularity = DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output,
                                        IDXGIOutput **ppOutput) final {
    InitReturnPtr(ppOutput);

    if (ppOutput == nullptr)
      return E_INVALIDARG;

    HMONITOR monitor = wsi::enumMonitors(Output);
    if (monitor == nullptr)
      return DXGI_ERROR_NOT_FOUND;

    *ppOutput = CreateOutput(this, monitor, options_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CheckInterfaceSupport(const GUID &guid, LARGE_INTEGER *umd_version) final {
    HRESULT hr = DXGI_ERROR_UNSUPPORTED;

    if (guid == __uuidof(IDXGIDevice) || guid == __uuidof(ID3D10Device) ||
        guid == __uuidof(ID3D10Device1))
      hr = S_OK;

    // We can't really reconstruct the version numbers
    // returned by Windows drivers from Metal
    if (SUCCEEDED(hr) && umd_version)
      umd_version->QuadPart = ~0ull;

    if (FAILED(hr)) {
      Logger::err("DXGI: CheckInterfaceSupport: Unsupported interface");
      Logger::err(str::format(guid));
    }

    return hr;
  }

  /* ml1007: the same defect as the budget-notification pair below -- both were
   * `assert(0 && "TODO")` with no return, compiling to a single `brk #1`. The
   * compiler says so plainly ("non-void function does not return a value"), and
   * rdr76 died on the sibling method, so this one is the same landmine one
   * vtable slot away.
   *
   * Unlike the budget notification, this is genuinely UNAVAILABLE rather than
   * merely static: there is no hardware content-protection path on this
   * platform, so there is no teardown to be notified about. DXGI_ERROR_UNSUPPORTED
   * is the truthful answer -- returning S_OK would promise a notification that
   * could never arrive and would mislead a caller that depends on it. */
  HRESULT STDMETHODCALLTYPE
  RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE event,
                                                       DWORD *cookie) override {
    (void)event;
    if (!cookie)
      return E_INVALIDARG;
    *cookie = 0;
    Logger::warn("DXGI: RegisterHardwareContentProtectionTeardownStatusEvent -- "
                 "no hardware content protection on this platform, returning "
                 "DXGI_ERROR_UNSUPPORTED");
    return DXGI_ERROR_UNSUPPORTED;
  }

  void STDMETHODCALLTYPE
  UnregisterHardwareContentProtectionTeardownStatus(DWORD cookie) override {
    /* Nothing was ever registered; the method returns void and cannot report. */
    Logger::warn(str::format("DXGI: UnregisterHardwareContentProtectionTeardownStatus "
                             "cookie=", cookie, " -- nothing was registered, ignored"));
  }

  HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(
      UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
      DXGI_QUERY_VIDEO_MEMORY_INFO *pVideoMemoryInfo) override {
    if (NodeIndex > 0 || !pVideoMemoryInfo)
      return E_INVALIDARG;

    if (MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
        MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
      return E_INVALIDARG;

    // we don't actually care about MemorySegmentGroup
    pVideoMemoryInfo->Budget = device_.recommendedMaxWorkingSetSize();
    pVideoMemoryInfo->CurrentUsage = device_.currentAllocatedSize();
    pVideoMemoryInfo->AvailableForReservation = 0;
    pVideoMemoryInfo->CurrentReservation =
        mem_reserved_[uint32_t(MemorySegmentGroup)];
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(
      UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
      UINT64 Reservation) override {
    if (NodeIndex > 0)
      return E_INVALIDARG;

    if (MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
        MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
      return E_INVALIDARG;

    mem_reserved_[uint32_t(MemorySegmentGroup)] = Reservation;
    return S_OK;
  }

  /* ml1007/ml2052: implement budget-change notification registration.
   *
   * These two were `assert(0 && "TODO")`. The Register variant additionally had
   * no return statement at all, so even with NDEBUG it falls off the end of a
   * non-void function. Compiled, the body is a single `brk #1`, and rdr76
   * traps there: RDR2 reaches DXGI adapter setup, calls this method through the
   * IDXGIAdapter3 vtable, and the process takes STATUS_ILLEGAL_INSTRUCTION
   * (c000001d) which nothing handles.
   *
   * Contract: register `event` to be signalled when the video-memory budget
   * changes, hand back a cookie that Unregister accepts. Registration is done
   * for real -- a unique cookie, the handle retained, and Unregister actually
   * removing it -- so the bookkeeping the caller observes is truthful.
   *
   * The iOS unix side now reports a DYNAMIC process budget: once shared CPU/GPU
   * memory approaches the jetsam line it lowers recommendedMaxWorkingSetSize.
   * ml2052 polls that authoritative value and signals every registered event
   * when it changes. This is how engines know to re-query and retire streaming
   * heaps; without it a title that queried only at startup kept its original
   * pool until iOS killed the app.
   *
   * ⛔ MEASURED, rdr77 vs rdr78 -- a clean single-variable A/B on the same
   * build, flipping only the file below:
   *   S_OK + event never fires  -> RDR2 HUNG on an auto-reset Event, infinite
   *                                wait, 8 minutes, no window
   *   DXGI_ERROR_UNSUPPORTED    -> swapchain created, 240 GPU flush cycles, the
   *                                game window appeared
   * So the caller really does wait on this event, and "register successfully and
   * never signal" is NOT an acceptable reading of a static budget -- Astra's
   * warning against returning fake success was correct and my first
   * implementation was wrong. Now that the event has a live producer,
   * registration is the default; Documents/madeira-dxgi-budget.txt = 0 is the
   * compatibility escape hatch. */
  HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(
      HANDLE event, DWORD *cookie) override {
    if (!event || !cookie)
      return E_INVALIDARG;

    if (!budget_notify_enabled()) {
      Logger::warn("DXGI: RegisterVideoMemoryBudgetChangeNotificationEvent "
                   "disabled by madeira-dxgi-budget.txt -> DXGI_ERROR_UNSUPPORTED");
      return DXGI_ERROR_UNSUPPORTED;
    }

    DWORD assigned;
    {
      std::lock_guard<std::mutex> lock(budget_mutex_);
      assigned = ++budget_cookie_seq_;
      budget_events_.emplace_back(assigned, event);
      if (!budget_notify_thread_.joinable()) {
        budget_notify_stop_.store(false, std::memory_order_release);
        try {
          budget_notify_thread_ = dxmt::thread([this]() { budget_notify_worker(); });
          budget_notify_thread_.set_priority(ThreadPriority::Lowest);
        } catch (...) {
          budget_events_.pop_back();
          *cookie = 0;
          Logger::warn("DXGI: could not start video-memory budget notifier");
          return E_FAIL;
        }
      }
    }
    *cookie = assigned;
    Logger::warn(str::format("DXGI: ml2052 registered live video-memory budget notification "
                             "cookie=", assigned));
    return S_OK;
  }

  void STDMETHODCALLTYPE
  UnregisterVideoMemoryBudgetChangeNotification(DWORD cookie) override {
    std::lock_guard<std::mutex> lock(budget_mutex_);
    for (auto it = budget_events_.begin(); it != budget_events_.end(); ++it) {
      if (it->first == cookie) {
        budget_events_.erase(it);
        Logger::warn(str::format("DXGI: unregistered video-memory budget "
                                 "notification cookie=", cookie));
        return;
      }
    }
    /* Windows ignores an unknown cookie here (the method returns void and
     * cannot report). Say so rather than silently doing nothing. */
    Logger::warn(str::format("DXGI: UnregisterVideoMemoryBudgetChangeNotification "
                             "for unknown cookie=", cookie, " -- ignored"));
  }

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() final { return device_; }
  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() final { return local_kmt_; }

private:
  /* ml2052: poll the same authoritative value QueryVideoMemoryInfo returns.
   * The unix side rate-limits task_info to 250 ms, so matching that cadence
   * adds no extra pressure. Auto-reset events coalesce repeated changes. */
  void budget_notify_worker() {
    uint64_t previous = device_.recommendedMaxWorkingSetSize();
    while (!budget_notify_stop_.load(std::memory_order_acquire)) {
      Sleep(250);
      if (budget_notify_stop_.load(std::memory_order_acquire))
        break;

      const uint64_t current = device_.recommendedMaxWorkingSetSize();
      if (!current || current == previous)
        continue;
      previous = current;

      size_t event_count;
      {
        std::lock_guard<std::mutex> lock(budget_mutex_);
        event_count = budget_events_.size();
        for (const auto &entry : budget_events_)
          SetEvent(entry.second);
      }
      Logger::warn(str::format("DXGI: ml2052 video-memory budget changed to ",
                               current >> 20, " MB; signalled ", event_count,
                               " registration(s)"));
    }
  }

  /* ml1007/ml2052: read once. A literal 0 disables the live notifier. */
  static bool budget_notify_enabled() {
    static const bool enabled = [] {
      const char *docs = std::getenv("MADEIRA_DOCS_DIR");
      if (!docs || !*docs)
        return true;
      std::string path = std::string(docs) + "/madeira-dxgi-budget.txt";
      FILE *f = std::fopen(path.c_str(), "r");
      if (!f)
        return true;
      int c = std::fgetc(f);
      std::fclose(f);
      return c != '0';
    }();
    return enabled;
  }

  WMT::Reference<WMT::Device> device_;
  D3DKMT_HANDLE local_kmt_ = 0;
  Com<IDXGIFactory> factory_;
  DxgiOptions options_;
  uint64_t mem_reserved_[2] = {0, 0};
  std::mutex budget_mutex_;
  std::vector<std::pair<DWORD, HANDLE>> budget_events_;
  DWORD budget_cookie_seq_ = 0;
  std::atomic<bool> budget_notify_stop_ {false};
  dxmt::thread budget_notify_thread_;
};

Com<IMTLDXGIAdapter> CreateAdapter(WMT::Device Device,
                                   IDXGIFactory2 *pFactory, Config &config) {
  return Com<IMTLDXGIAdapter>::transfer(
      new MTLDXGIAdatper(Device, pFactory, config));
}

} // namespace dxmt
