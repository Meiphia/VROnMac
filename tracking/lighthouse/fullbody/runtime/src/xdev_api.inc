// XR_MNDX_xdev_space version 1. Device lists are snapshots, as in Monado.
static XdevList* FindXdevList(XrXDevListMNDX handle) {
    if (!g_instance || !g_instance->session) return nullptr;
    auto* list=reinterpret_cast<XdevList*>(handle);
    return g_instance->session->xdevLists.count(list) ? list : nullptr;
}
static const VrcmTracker* FindXdev(XdevList* list,uint64_t id) {
    for (const auto& device:list->devices) if (device.id==id) return &device;
    return nullptr;
}
static XrResult XRAPI_CALL rt_xrCreateXDevListMNDX(XrSession session,const XrCreateXDevListInfoMNDX* info,XrXDevListMNDX* out) {
    LOCK(); GET_SESSION(s,session);
    if (!info || !out || info->type!=XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX) return XR_ERROR_VALIDATION_FAILURE;
    *out=XR_NULL_HANDLE;
    if (!s->instance->enabledExts.count(XR_MNDX_XDEV_SPACE_EXTENSION_NAME)) return XR_ERROR_FUNCTION_UNSUPPORTED;
    auto* list=new XdevList(); list->session=s;
    list->devices=BridgeClient::Get().Trackers(&list->generation);
    // Discovery follows connection setup closely. Give the independent TICK
    // receiver a bounded interval to ingest metadata already being published.
    for(int attempt=0;list->devices.empty() && attempt<20;++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        list->devices=BridgeClient::Get().Trackers(&list->generation);
    }
    s->xdevLists.insert(list); *out=reinterpret_cast<XrXDevListMNDX>(list);
    VLOG("Created xdev list with %u trackers",(unsigned)list->devices.size());
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_xrGetXDevListGenerationNumberMNDX(XrXDevListMNDX handle,uint64_t* out) {
    LOCK(); auto* list=FindXdevList(handle);
    if (!list) return XR_ERROR_HANDLE_INVALID;
    if (!out) return XR_ERROR_VALIDATION_FAILURE;
    *out=list->generation; return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_xrEnumerateXDevsMNDX(XrXDevListMNDX handle,uint32_t capacity,uint32_t* count,XrXDevIdMNDX* out) {
    LOCK(); auto* list=FindXdevList(handle);
    if (!list) return XR_ERROR_HANDLE_INVALID;
    std::vector<XrXDevIdMNDX> ids;
    for (const auto& device:list->devices) ids.push_back(device.id);
    return TwoCall<XrXDevIdMNDX>(capacity,count,out,ids);
}
static XrResult XRAPI_CALL rt_xrGetXDevPropertiesMNDX(XrXDevListMNDX handle,const XrGetXDevInfoMNDX* info,XrXDevPropertiesMNDX* out) {
    LOCK(); auto* list=FindXdevList(handle);
    if (!list) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out || info->type!=XR_TYPE_GET_XDEV_INFO_MNDX || out->type!=XR_TYPE_XDEV_PROPERTIES_MNDX)
        return XR_ERROR_VALIDATION_FAILURE;
    auto* device=FindXdev(list,info->id);
    if (!device) return XR_ERROR_RUNTIME_FAILURE;
    snprintf(out->name,sizeof(out->name),"%s",device->name);
    snprintf(out->serial,sizeof(out->serial),"%s",device->serial);
    out->canCreateSpace=XR_TRUE;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_xrDestroyXDevListMNDX(XrXDevListMNDX handle) {
    LOCK(); auto* list=FindXdevList(handle);
    if (!list) return XR_ERROR_HANDLE_INVALID;
    list->session->xdevLists.erase(list); delete list; return XR_SUCCESS;
}
static XrResult XRAPI_CALL rt_xrCreateXDevSpaceMNDX(XrSession session,const XrCreateXDevSpaceInfoMNDX* info,XrSpace* out) {
    LOCK(); GET_SESSION(s,session);
    if (!info || !out || info->type!=XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX) return XR_ERROR_VALIDATION_FAILURE;
    *out=XR_NULL_HANDLE;
    auto* list=FindXdevList(info->xdevList);
    if (!list || list->session!=s) return XR_ERROR_HANDLE_INVALID;
    if (!FindXdev(list,info->id)) return XR_ERROR_RUNTIME_FAILURE;
    const auto& q=info->offset.orientation; const auto& p=info->offset.position;
    double norm=double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w;
    if (!std::isfinite(norm) || std::abs(norm-1)>0.01 || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        return XR_ERROR_POSE_INVALID;
    auto* space=new Space(); space->session=s; space->kind=SK_TRACKER;
    space->trackerId=info->id; space->offset=info->offset;
    s->spaces.insert(space); *out=reinterpret_cast<XrSpace>(space); return XR_SUCCESS;
}
