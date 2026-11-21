/* Network-cost object for hosts without billing information: report Unknown,
 * while preserving actual NLM cost flags when the host supplies them. */
struct connection_cost
{
    IConnectionCost IConnectionCost_iface;
    LONG ref;
    DWORD flags;
};
static struct connection_cost *cost_impl(IConnectionCost *iface)
{ return CONTAINING_RECORD(iface, struct connection_cost, IConnectionCost_iface); }
static ULONG WINAPI cost_AddRef(IConnectionCost *iface)
{ return InterlockedIncrement(&cost_impl(iface)->ref); }
static ULONG WINAPI cost_Release(IConnectionCost *iface)
{
    struct connection_cost *impl = cost_impl(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);
    if (!ref) free(impl);
    return ref;
}
static HRESULT WINAPI cost_QueryInterface(IConnectionCost *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) || IsEqualGUID(iid, &IID_IConnectionCost))
    { *out = iface; cost_AddRef(iface); return S_OK; }
    return E_NOINTERFACE;
}
static HRESULT WINAPI cost_GetIids(IConnectionCost *iface, ULONG *count, IID **ids)
{
    if (!count || !ids) return E_POINTER;
    *count = 0; *ids = CoTaskMemAlloc(sizeof(IID));
    if (!*ids) return E_OUTOFMEMORY;
    **ids = IID_IConnectionCost; *count = 1; return S_OK;
}
static HRESULT WINAPI cost_GetRuntimeClassName(IConnectionCost *iface, HSTRING *name)
{
    const WCHAR value[] = L"Windows.Networking.Connectivity.ConnectionCost";
    if (!name) return E_POINTER;
    return WindowsCreateString(value, ARRAYSIZE(value)-1, name);
}
static HRESULT WINAPI cost_GetTrustLevel(IConnectionCost *iface, TrustLevel *trust)
{ if (!trust) return E_POINTER; *trust = BaseTrust; return S_OK; }
static HRESULT WINAPI cost_get_NetworkCostType(IConnectionCost *iface, NetworkCostType *value)
{
    DWORD flags = cost_impl(iface)->flags;
    if (!value) return E_POINTER;
    *value = NetworkCostType_Unknown;
    if (flags & NLM_CONNECTION_COST_UNRESTRICTED) *value = NetworkCostType_Unrestricted;
    else if (flags & NLM_CONNECTION_COST_FIXED) *value = NetworkCostType_Fixed;
    else if (flags & NLM_CONNECTION_COST_VARIABLE) *value = NetworkCostType_Variable;
    return S_OK;
}
static HRESULT WINAPI cost_get_Roaming(IConnectionCost *iface, boolean *value)
{ if (!value) return E_POINTER; *value = !!(cost_impl(iface)->flags & NLM_CONNECTION_COST_ROAMING); return S_OK; }
static HRESULT WINAPI cost_get_OverDataLimit(IConnectionCost *iface, boolean *value)
{ if (!value) return E_POINTER; *value = !!(cost_impl(iface)->flags & NLM_CONNECTION_COST_OVERDATALIMIT); return S_OK; }
static HRESULT WINAPI cost_get_ApproachingDataLimit(IConnectionCost *iface, boolean *value)
{ if (!value) return E_POINTER; *value = !!(cost_impl(iface)->flags & NLM_CONNECTION_COST_APPROACHINGDATALIMIT); return S_OK; }
static IConnectionCostVtbl cost_vtbl = {
    cost_QueryInterface, cost_AddRef, cost_Release, cost_GetIids,
    cost_GetRuntimeClassName, cost_GetTrustLevel, cost_get_NetworkCostType,
    cost_get_Roaming, cost_get_OverDataLimit, cost_get_ApproachingDataLimit
};
static HRESULT WINAPI connection_profile_GetConnectionCost(IConnectionProfile *iface, IConnectionCost **value)
{
    struct connection_profile *profile = impl_from_IConnectionProfile(iface);
    struct connection_cost *cost;
    INetworkCostManager *manager;
    DWORD flags = NLM_CONNECTION_COST_UNKNOWN;
    if (!value) return E_POINTER;
    *value = NULL;
    cost = calloc(1, sizeof(*cost));
    if (!cost) return E_OUTOFMEMORY;
    if (SUCCEEDED(INetworkListManager_QueryInterface(profile->network_list_manager, &IID_INetworkCostManager, (void **)&manager)))
    {
        if (FAILED(INetworkCostManager_GetCost(manager, &flags, NULL))) flags = NLM_CONNECTION_COST_UNKNOWN;
        INetworkCostManager_Release(manager);
    }
    cost->IConnectionCost_iface.lpVtbl = &cost_vtbl;
    cost->ref = 1; cost->flags = flags;
    *value = &cost->IConnectionCost_iface;
    return S_OK;
}
