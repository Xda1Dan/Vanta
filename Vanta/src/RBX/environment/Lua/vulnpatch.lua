local __vanta_vp_t0 = os.clock()
-- Vulnerability blocklist, applied at startup.
--
-- For each entry, the owning object is resolved (a service, or `game` for the
-- DataModel) and every listed member is blocked for executor threads through its
-- reflection descriptor - see src/vulnpatch.hpp. The engine's own callers are
-- untouched; only our threads are refused. Missing members (a build that does not
-- expose one) are logged and skipped, never fatal.
--
-- This list is the union of every reference executor's blocklist and the members
-- Myriad's suite checks; it ships encrypted with the rest of the bootstrap.

local patch = __vanta_vulnpatch
local passes = __vanta_vulnpatch_passes

-- Descriptors are process-global and the vtable clone is permanent, so once one
-- full pass has completed in this process every later join - a teleport, a
-- rejoin - is already covered. Resolving all ~220 members again only found each
-- one already patched, at ~54 ms a join (journal, 2026-09-03). The primitive
-- also re-reads this VM's global_State on every call, which is what keeps the
-- block check recognising the new VM's threads after a teleport - so it is
-- asked on every join, skipped or not.
if passes and passes() > 0 then
	__vanta_log("BOOTSTRAP vulnpatch already complete in this process; skipped")
	return
end

local function block(obj, members)
	if not obj then return end
	for i = 1, #members do
		patch(obj, members[i])
	end
end

local function service(name)
	local ok, s = pcall(game.GetService, game, name)
	if ok and s then return s end
	ok, s = pcall(game.FindService, game, name)
	if ok then return s end
	return nil
end

-- The DataModel itself.
local __t = os.clock()
block(game, {
	"Shutdown", "Load", "OpenLogsFolder", "OpenScreenshotsFolder", "OpenVideosFolder",
})
if (os.clock() - __t) * 1000 >= 2 then
	__vanta_log(string.format("VULNTIME DataModel %.1f ms", (os.clock() - __t) * 1000))
end

local services = {
	UserInputService = { "GetPasteText" },
	TestService = { "Run", "Require", "RunAsync" },
	TeleportService = { "GetPlayerPlaceInstanceAsync" },
	WebViewService = { "CloseWindow", "MutateWindow", "OpenWindow" },
	AccountService = {
		"GetCredentialsHeaders", "GetDeviceAccessToken",
		"GetDeviceIntegrityToken", "GetDeviceIntegrityTokenYield",
	},
	AnalyticsService = {
		"FireInGameEconomyEvent", "FireLogEvent", "FireEvent",
		"FireCustomEvent", "LogEconomyEvent",
	},
	AnimationFromVideoCreatorService = { "CreateJob", "DownloadJobResult", "FullProcess" },
	CaptureService = {
		"CaptureScreenshot", "DeleteCapture", "GetCaptureFilePathAsync", "CreatePostAsync",
		"SaveCaptureToExternalStorage", "SaveCapturesToExternalStorageAsync",
		"GetCaptureSizeAsync", "GetCaptureStorageSizeAsync", "PromptSaveCapturesToGallery",
		"PromptShareCapture", "RetrieveCaptures", "SaveScreenshotCapture",
		"GetCaptureUploadDataAsync", "DeleteCapturesAsync",
	},
	InsertService = { "GetLocalFileContents" },
	SafetyService = { "TakeScreenshot" },
	HttpRbxApiService = {
		"PostAsync", "PostAsyncFullUrl", "GetAsyncFullUrl", "GetAsync",
		"RequestAsync", "RequestLimitedAsync",
	},
	UGCValidationService = { "FetchAssetWithFormat", "RegisterUGCValidationFunction" },
	MarketplaceService = {
		"GetUserSubscriptionStatusAsync", "PromptNativePurchase", "PerformCancelSubscription",
		"PerformPurchaseV2", "PrepareCollectiblesPurchase", "PromptCancelSubscription",
		"ReportAssetSale", "GetUserSubscriptionDetailsInternalAsync", "PerformPurchase",
		"PromptBundlePurchase", "PromptGamePassPurchase", "PromptProductPurchase",
		"PromptPurchase", "PromptRobloxPurchase", "PromptThirdPartyPurchase", "GetRobuxBalance",
		"PromptBulkPurchase", "PerformBulkPurchase", "PerformSubscriptionPurchase",
		"PerformSubscriptionPurchaseV2", "PromptCollectiblesPurchase",
		"PromptNativePurchaseWithLocalPlayer", "PromptPremiumPurchase", "PromptSubscriptionPurchase",
		"GetUserSubscriptionPaymentHistoryAsync",
	},
	GuiService = {
		"OpenBrowserWindow", "OpenNativeOverlay", "BroadcastNotification", "SetPurchasePromptIsShown",
	},
	DataModelPatchService = { "RegisterPatch", "UpdatePatch" },
	OpenCloudService = { "HttpRequestAsync", "GetApiV1", "InvokeAsync", "RegisterOpenCloud" },
	EventIngestService = {
		"SendEventDeferred", "SetRBXEvent", "SetRBXEventStream", "SendEventImmediately",
	},
	CoreScriptSyncService = { "GetScriptFilePath" },
	ScriptContext = { "AddCoreScriptLocal" },
	ScriptProfilerService = { "SaveScriptProfilingData" },
	BrowserService = {
		"EmitHybridEvent", "OpenWeChatAuthWindow", "ExecuteJavaScript", "OpenBrowserWindow",
		"OpenNativeOverlay", "ReturnToJavaScript", "CopyAuthCookieFromBrowserToEngine", "SendCommand",
	},
	MessageBusService = {
		"Call", "GetLast", "GetMessageId", "GetProtocolMethodRequestMessageId",
		"GetProtocolMethodResponseMessageId", "MakeRequest", "Publish",
		"PublishProtocolMethodRequest", "PublishProtocolMethodResponse", "Subscribe",
		"SubscribeToProtocolMethodRequest", "SubscribeToProtocolMethodResponse", "SetRequestHandler",
	},
	AppUpdateService = { "PerformManagedUpdate" },
	MessagingService = { "PublishAsync", "SubscribeAsync" },
	ContentProvider = { "SetBaseUrl" },
	AppStorageService = { "Flush", "GetItem", "SetItem" },
	IXPService = {
		"GetBrowserTrackerLayerVariables", "GetRegisteredUserLayersToStatus", "GetUserLayerVariables",
		"GetUserStatusForLayer", "InitializeUserLayers", "LogBrowserTrackerLayerExposure",
		"LogUserLayerExposure", "RegisterUserLayers",
	},
	HttpService = {
		"RequestInternal", "GetAsync", "RequestAsync", "CreateWebStreamClient",
		"PostAsync", "SetHttpEnabled",
	},
	SessionService = {
		"AcquireContextFocus", "GenerateSessionInfoString", "GetCreatedTimestampUtcMs", "GetMetadata",
		"GetRootSID", "GetSessionTag", "IsContextFocused", "ReleaseContextFocus", "RemoveMetadata",
		"RemoveSession", "RemoveSessionsWithMetadataKey", "ReplaceSession", "SessionExists",
		"SetMetadata", "SetSession", "GetSessionID",
	},
	ContextActionService = { "CallFunction", "BindCoreActivate" },
	CommerceService = {
		"PromptCommerceProductPurchase", "PromptRealWorldCommerceBrowser",
		"UserEligibleForRealWorldCommerceAsync",
	},
	OmniRecommendationsService = { "ClearSessionId", "MakeRequest" },
	Players = { "ReportAvatarAbuse", "ReportChatAbuse", "ReportAbuse", "ReportAbuseV3" },
	PlatformCloudStorageService = { "GetUserDataAsync", "SetUserDataAsync" },
	CoreGui = { "TakeScreenshot", "ToggleRecording" },
	LinkingService = {
		"DetectUrl", "GetAndClearLastPendingUrl", "GetLastLuaUrl", "IsUrlRegistered", "OpenUrl",
		"RegisterLuaUrl", "StartLuaUrlDelivery", "StopLuaUrlDelivery",
		"SupportsSwitchToSettingsApp", "SwitchToSettingsApp",
	},
	RbxAnalyticsService = {
		"GetSessionId", "ReleaseRBXEventStream", "SendEventDeferred", "SendEventImmediately",
		"SetRBXEvent", "SetRBXEventStream", "TrackEvent", "TrackEventWithArgs",
	},
	AvatarEditorService = {
		"NoPromptSetFavorite", "NoPromptUpdateOutfit", "PerformCreateOutfitWithDescription",
		"PerformDeleteOutfit", "PerformRenameOutfit", "PerformSaveAvatarWithDescription",
		"PerformSetFavorite", "PerformUpdateOutfit", "PromptAllowInventoryReadAccess",
		"PromptCreateOutfit", "PromptDeleteOutfit", "PromptRenameOutfit", "PromptSaveAvatar",
		"PromptSetFavorite", "PromptUpdateOutfit", "SetAllowInventoryReadAccess",
		"SignalCreateOutfitFailed", "SignalCreateOutfitPermissionDenied", "SignalDeleteOutfitFailed",
		"SignalDeleteOutfitPermissionDenied", "SignalRenameOutfitFailed",
		"SignalRenameOutfitPermissionDenied", "SignalSaveAvatarPermissionDenied",
		"SignalSetFavoriteFailed", "SignalSetFavoritePermissionDenied", "SignalUpdateOutfitFailed",
		"SignalUpdateOutfitPermissionDenied", "NoPromptSaveAvatarThumbnailCustomization",
		"NoPromptSaveAvatar", "NoPromptRenameOutfit", "NoPromptDeleteOutfit", "NoPromptCreateOutfit",
	},
	WebSocketService = { "CreateClient" },
	FileManagerService = {
		"ListFilesInFolderAsync", "OpenFileInWebBrowser", "OpenFolder", "RevealFileInFolder",
	},
	MicroProfilerService = { "ProcessCommand", "DumpToFileAsync" },
}

for name, members in pairs(services) do
	local t = os.clock()
	block(service(name), members)
	local ms = (os.clock() - t) * 1000
	if ms >= 2 then
		__vanta_log(string.format("VULNTIME %s %.1f ms for %d members", name, ms, #members))
	end
end

if passes then passes(true) end

__vanta_log(string.format("BOOTSTRAP vulnpatch ran in %.1f ms", (os.clock() - __vanta_vp_t0) * 1000))
