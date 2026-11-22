#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

typedef HRESULT (*QueryApi)(const GUID&, const GUID&, void**);

static int g_failures = 0;
static void Check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) g_failures++;
}

static QueryApi g_query;
static std::string g_table;
static bool LoadTable() {
  std::ifstream file("table.json", std::ios::binary);
  if (!file) return false;
  std::ostringstream text;
  text << file.rdbuf();
  g_table = text.str();
  return !g_table.empty();
}

struct Block {
  size_t begin = std::string::npos;
  size_t end = std::string::npos;
  bool found() const { return begin != std::string::npos; }
};
static Block InterfaceBlock(const char* iface) {
  Block b;
  std::string key = std::string("\"name\": \"") + iface + "\",";
  b.begin = g_table.find(key);
  if (b.begin == std::string::npos) return b;
  b.end = g_table.find("\"file\": ", b.begin);
  if (b.end == std::string::npos) b.end = g_table.size();
  return b;
}
static int SlotIn(const Block& b, const char* method) {
  std::string key = std::string("\"name\": \"") + method + "\",";
  size_t at = g_table.find(key, b.begin + 1);
  if (at == std::string::npos || at >= b.end) return -1;
  if (g_table.find(key, at + 1) < b.end) return -2;
  size_t slot = g_table.find("\"slot\": ", at);
  size_t next = g_table.find("\"name\": ", at + key.size());
  if (slot == std::string::npos || slot >= b.end || (next != std::string::npos && slot > next)) return -1;
  return std::atoi(g_table.c_str() + slot + 8);
}
static int Slots(const Block& b) {
  size_t at = g_table.find("\"nslots\": ", b.begin);
  if (at == std::string::npos) return 0;
  return std::atoi(g_table.c_str() + at + 10);
}
static bool Uuid(const Block& b, GUID* guid) {
  size_t at = g_table.find("\"uuid\": \"", b.begin);
  if (at == std::string::npos || at >= b.end) return false;
  std::string text = g_table.substr(at + 9, 36);
  unsigned long d1 = 0;
  unsigned d2 = 0, d3 = 0, d4[8] = {};
  if (std::sscanf(text.c_str(), "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &d4[0], &d4[1], &d4[2], &d4[3], &d4[4], &d4[5],
                  &d4[6], &d4[7]) != 11)
    return false;
  guid->Data1 = d1;
  guid->Data2 = static_cast<unsigned short>(d2);
  guid->Data3 = static_cast<unsigned short>(d3);
  for (int i = 0; i < 8; i++) guid->Data4[i] = static_cast<unsigned char>(d4[i]);
  return true;
}
static bool NamedAnywhere(const char* method) {
  return g_table.find(std::string("\"name\": \"") + method + "\",") != std::string::npos;
}

struct Documented {
  const char* iface;
  std::vector<const char*> methods;
};
static const Documented kDocumented[] = {
    {"IXAccessibilityImpl",
     {"XClosedCaptionGetProperties", "XClosedCaptionSetEnabled", "XHighContrastGetMode", "XSpeechToTextSetPositionHint", "XSpeechToTextSendString",
      "XSpeechSynthesizerEnumerateInstalledVoices", "XSpeechSynthesizerCreate", "XSpeechSynthesizerCloseHandle", "XSpeechSynthesizerSetDefaultVoice",
      "XSpeechSynthesizerSetCustomVoice", "XSpeechSynthesizerCreateStreamFromText", "XSpeechSynthesizerCloseStreamHandle",
      "XSpeechSynthesizerGetStreamDataSize", "XSpeechSynthesizerGetStreamData", "XSpeechToTextBeginHypothesisString",
      "XSpeechToTextUpdateHypothesisString", "XSpeechToTextFinalizeHypothesisString", "XSpeechToTextCancelHypothesisString",
      "XSpeechSynthesizerCreateStreamFromSsml"}},
    {"IXAppCaptureMetadataImpl",
     {"XAppBroadcastIsAppBroadcasting", "XAppBroadcastShowUI", "XAppBroadcastGetStatus", "XAppBroadcastRegisterIsAppBroadcastingChanged",
      "XAppBroadcastUnregisterIsAppBroadcastingChanged", "XAppCaptureMetadataAddStringEvent", "XAppCaptureMetadataAddInt32Event",
      "XAppCaptureMetadataAddDoubleEvent", "XAppCaptureMetadataStartStringState", "XAppCaptureMetadataStartInt32State",
      "XAppCaptureMetadataStartDoubleState", "XAppCaptureMetadataStopState", "XAppCaptureMetadataStopAllStates",
      "XAppCaptureMetadataRemainingStorageBytesAvailable", "XAppCaptureRegisterMetadataPurged", "XAppCaptureUnRegisterMetadataPurged"}},
    {"IXAppCaptureImpl",
     {"XAppCaptureTakeDiagnosticScreenshot", "XAppCaptureRecordDiagnosticClip", "XAppCaptureTakeScreenshot", "XAppCaptureOpenScreenshotStream",
      "XAppCaptureReadScreenshotStream", "XAppCaptureCloseScreenshotStream", "XAppCaptureEnableRecord", "XAppCaptureDisableRecord"}},
    {"IXAppCaptureImpl2",
     {"XAppCaptureGetVideoCaptureSettings", "XAppCaptureRecordTimespan", "XAppCaptureReadLocalStream", "XAppCaptureCloseLocalStream"}},
    {"IXAppCaptureImpl3",
     {"XAppCaptureStartUserRecord", "XAppCaptureStopUserRecord"}},
    {"IXAppCaptureImpl4",
     {"XAppCaptureCancelUserRecord"}},
    {"IXThreadingImpl",
     {"XAsyncGetStatus", "XAsyncGetResultSize", "XAsyncCancel", "XAsyncRun", "XTaskQueueCreate", "XTaskQueueCreateComposite", "XTaskQueueGetPort",
      "XTaskQueueDuplicateHandle", "XTaskQueueDispatch", "XTaskQueueCloseHandle", "XTaskQueueSubmitCallback", "XTaskQueueSubmitDelayedCallback",
      "XTaskQueueRegisterWaiter", "XTaskQueueUnregisterWaiter", "XTaskQueueTerminate", "XTaskQueueRegisterMonitor", "XTaskQueueUnregisterMonitor",
      "XTaskQueueGetCurrentProcessTaskQueue", "XTaskQueueSetCurrentProcessTaskQueue", "XThreadSetTimeSensitive", "XThreadAssertNotTimeSensitive",
      "XThreadIsTimeSensitive"}},
    {"IXDisplayImpl",
     {"XDisplayTryEnableHdrMode"}},
    {"IXLauncherImpl",
     {"XLaunchUri", "XDisplayAcquireTimeoutDeferral", "XDisplayCloseTimeoutDeferralHandle"}},
    {"IXErrorImpl",
     {"XErrorSetCallback", "XErrorSetOptions"}},
    {"IXGameImpl",
     {"XGameGetXboxTitleId"}},
    {"IXGameImpl2",
     {"XLaunchNewGame"}},
    {"IXGameImpl3",
     {"XLaunchRestartOnCrash"}},
    {"IXGameActivationImpl",
     {"XGameActivationRegisterForEvent", "XGameActivationUnregisterForEvent"}},
    {"IXGameEventImpl",
     {"XGameEventWrite"}},
    {"IXGameInviteImpl",
     {"XGameInviteRegisterForEvent", "XGameInviteUnregisterForEvent"}},
    {"IXGameProtocolImpl",
     {"XGameProtocolRegisterForActivation", "XGameProtocolUnregisterForActivation"}},
    {"IXGameRuntimeFeatureImpl",
     {"XGameRuntimeIsFeatureAvailable"}},
    {"IXGameSaveImpl",
     {"XGameSaveInitializeProvider", "XGameSaveInitializeProviderAsync", "XGameSaveInitializeProviderResult", "XGameSaveCloseProvider",
      "XGameSaveGetRemainingQuota", "XGameSaveGetRemainingQuotaAsync", "XGameSaveGetRemainingQuotaResult", "XGameSaveDeleteContainer",
      "XGameSaveDeleteContainerAsync", "XGameSaveDeleteContainerResult", "XGameSaveGetContainerInfo", "XGameSaveEnumerateContainerInfo",
      "XGameSaveEnumerateContainerInfoByName", "XGameSaveCreateContainer", "XGameSaveCloseContainer", "XGameSaveEnumerateBlobInfo",
      "XGameSaveEnumerateBlobInfoByName", "XGameSaveReadBlobData", "XGameSaveReadBlobDataAsync", "XGameSaveReadBlobDataResult",
      "XGameSaveCreateUpdate", "XGameSaveCloseUpdate", "XGameSaveSubmitBlobWrite", "XGameSaveSubmitBlobDelete", "XGameSaveSubmitUpdate",
      "XGameSaveSubmitUpdateAsync", "XGameSaveSubmitUpdateResult"}},
    {"IXGameSaveImpl2",
     {"XGameSaveFilesGetFolderWithUiAsync", "XGameSaveFilesGetFolderWithUiResult", "XGameSaveFilesGetRemainingQuota"}},
    {"IXGameStreamingImpl",
     {"XGameStreamingInitialize", "XGameStreamingUninitialize", "XGameStreamingIsStreaming", "XGameStreamingRegisterClientPropertiesChanged",
      "XGameStreamingUnregisterClientPropertiesChanged", "XGameStreamingGetStreamPhysicalDimensions", "XGameStreamingGetClientCount",
      "XGameStreamingGetClients", "XGameStreamingGetConnectionState", "XGameStreamingRegisterConnectionStateChanged",
      "XGameStreamingUnregisterConnectionStateChanged", "XGameStreamingGetStreamAddedLatency", "XGameStreamingGetServerLocationNameSize",
      "XGameStreamingGetServerLocationName", "XGameStreamingHideTouchControls", "XGameStreamingShowTouchControlLayout",
      "XGameStreamingHideTouchControlsOnClient", "XGameStreamingShowTouchControlLayoutOnClient", "XGameStreamingIsTouchInputEnabled",
      "XGameStreamingGetLastFrameDisplayed", "XGameStreamingGetAssociatedFrame", "XGameStreamingGetGamepadPhysicality",
      "XGameStreamingUpdateTouchControlsState", "XGameStreamingUpdateTouchControlsStateOnClient", "XGameStreamingShowTouchControlsWithStateUpdate",
      "XGameStreamingShowTouchControlsWithStateUpdateOnClient", "XGameStreamingGetTouchBundleVersionNameSize", "XGameStreamingGetTouchBundleVersion",
      "XGameStreamingGetClientIPAddress"}},
    {"IXGameStreamingImpl2",
     {"XGameStreamingGetSessionId"}},
    {"IXGameStreamingImpl3",
     {"XGameStreamingGetDisplayDetails", "XGameStreamingSetResolution"}},
    {"IXGameUiImpl",
     {"XGameUiShowMessageDialogAsync", "XGameUiShowMessageDialogResult", "XGameUiShowSendGameInviteAsync", "XGameUiShowSendGameInviteResult",
      "XGameUiShowPlayerProfileCardAsync", "XGameUiShowPlayerProfileCardResult", "XGameUiShowAchievementsAsync", "XGameUiShowAchievementsResult",
      "XGameUiShowPlayerPickerAsync", "XGameUiShowPlayerPickerResultCount", "XGameUiShowPlayerPickerResult", "XGameUiShowErrorDialogAsync",
      "XGameUiShowErrorDialogResult", "XGameUiSetNotificationPositionHint", "XGameUiShowTextEntryAsync", "XGameUiShowTextEntryResultSize",
      "XGameUiShowTextEntryResult", "XGameUiShowWebAuthenticationAsync", "XGameUiShowWebAuthenticationResultSize",
      "XGameUiShowWebAuthenticationResult", "XGameUiShowWebAuthenticationWithOptionsAsync"}},
    {"IXGameUiImpl2",
     {"XGameUiShowMultiplayerActivityGameInviteAsync", "XGameUiShowMultiplayerActivityGameInviteResult", "XGameUiTextEntryOpen",
      "XGameUiTextEntryClose", "XGameUiTextEntryGetState", "XGameUiTextEntryGetExtents", "XGameUiTextEntryUpdatePositionHint",
      "XGameUiTextEntryUpdateVisibility"}},
    {"IXGameUiImpl3",
     {"XGameUiShowStateShareAsync", "XGameUiShowStateShareResult"}},
    {"IXGameUiImpl4",
     {"XGameUiSetUiCallbacks", "XGameUiSetMessageDialogUiResponse", "XGameUiSetPlayerPickerUiResponse", "XGameUiSetTextEntryUiResponse",
      "XGameUiSetPlayerProfileCardUiResponse", "XGameUiSetSendGameInviteUiResponse", "XGameUiSetAchievementsUiResponse",
      "XGameUiSetMultiplayerActivityGameInviteUiResponse", "XGameUiSetErrorDialogUiResponse"}},
    {"IXNetworkingImpl",
     {"XNetworkingQueryPreferredLocalUdpMultiplayerPort", "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync",
      "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult", "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged",
      "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged", "XNetworkingQuerySecurityInformationForUrlAsync",
      "XNetworkingQuerySecurityInformationForUrlAsyncResultSize", "XNetworkingQuerySecurityInformationForUrlAsyncResult",
      "XNetworkingQuerySecurityInformationForUrlUtf16Async", "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize",
      "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult", "XNetworkingVerifyServerCertificate", "XNetworkingGetConnectivityHint",
      "XNetworkingRegisterConnectivityHintChanged", "XNetworkingUnregisterConnectivityHintChanged"}},
    {"IXNetworkingImpl2",
     {"XNetworkingQueryConfigurationSetting", "XNetworkingSetConfigurationSetting", "XNetworkingQueryStatistics"}},
    {"IXPackageImpl",
     {"XPackageGetCurrentProcessPackageIdentifier", "XPackageIsPackagedProcess", "XPackageCreateInstallationMonitor",
      "XPackageCloseInstallationMonitorHandle", "XPackageGetInstallationProgress", "XPackageUpdateInstallationMonitor",
      "XPackageRegisterInstallationProgressChanged", "XPackageUnregisterInstallationProgressChanged", "XPackageGetUserLocale",
      "XPackageFindChunkAvailability", "XPackageEnumerateChunkAvailability", "XPackageChangeChunkInstallOrder", "XPackageInstallChunks",
      "XPackageInstallChunksAsync", "XPackageInstallChunksResult", "XPackageEstimateDownloadSize", "XPackageUninstallChunks",
      "XPackageUnregisterPackageInstalled", "XPackageGetMountPathSize", "XPackageGetMountPath", "XPackageCloseMountHandle",
      "XPackageEnumeratePackages", "XPackageRegisterPackageInstalled", "XPackageGetWriteStats", "XPackageUninstallUWPInstance",
      "XPackageEnumerateFeatures", "XPackageUninstallPackage"}},
    {"IXPackageImpl2",
     {"XPackageEnumeratePackages", "XPackageRegisterPackageInstalled", "XPackageMountWithUiAsync", "XPackageMountWithUiResult"}},
    {"IXPackageImpl3",
     {"XPackageEnumeratePackages", "XPackageRegisterPackageInstalled"}},
    {"IXPackageImpl4",
     {"XPackageGetKind"}},
    {"IXPersistentLocalStorageImpl",
     {"XPersistentLocalStorageGetPathSize", "XPersistentLocalStorageGetPath", "XPersistentLocalStorageGetSpaceInfo",
      "XPersistentLocalStoragePromptUserForSpaceAsync", "XPersistentLocalStoragePromptUserForSpaceResult"}},
    {"IXPersistentLocalStorageImpl3",
     {"XPersistentLocalStorageMountForPackage"}},
    {"IXStoreImpl",
     {"XStoreCreateContext", "XStoreCloseContextHandle", "XStoreQueryAssociatedProductsAsync", "XStoreQueryAssociatedProductsResult",
      "XStoreQueryProductsAsync", "XStoreQueryProductsResult", "XStoreQueryEntitledProductsAsync", "XStoreQueryEntitledProductsResult",
      "XStoreQueryProductForCurrentGameAsync", "XStoreQueryProductForCurrentGameResult", "XStoreQueryProductForPackageAsync",
      "XStoreQueryProductForPackageResult", "XStoreEnumerateProductsQuery", "XStoreProductsQueryHasMorePages", "XStoreProductsQueryNextPageAsync",
      "XStoreProductsQueryNextPageResult", "XStoreCloseProductsQueryHandle", "XStoreAcquireLicenseForPackageAsync",
      "XStoreAcquireLicenseForPackageResult", "XStoreIsLicenseValid", "XStoreCloseLicenseHandle", "XStoreCanAcquireLicenseForStoreIdAsync",
      "XStoreCanAcquireLicenseForStoreIdResult", "XStoreCanAcquireLicenseForPackageAsync", "XStoreCanAcquireLicenseForPackageResult",
      "XStoreQueryGameLicenseAsync", "XStoreQueryGameLicenseResult", "XStoreQueryAddOnLicensesAsync", "XStoreQueryAddOnLicensesResultCount",
      "XStoreQueryAddOnLicensesResult", "XStoreQueryConsumableBalanceRemainingAsync", "XStoreQueryConsumableBalanceRemainingResult",
      "XStoreReportConsumableFulfillmentAsync", "XStoreReportConsumableFulfillmentResult", "XStoreGetUserCollectionsIdAsync",
      "XStoreGetUserCollectionsIdResultSize", "XStoreGetUserCollectionsIdResult", "XStoreGetUserPurchaseIdAsync",
      "XStoreGetUserPurchaseIdResultSize", "XStoreGetUserPurchaseIdResult", "XStoreQueryLicenseTokenAsync", "XStoreQueryLicenseTokenResultSize",
      "XStoreQueryLicenseTokenResult", "XStoreShowPurchaseUIAsync", "XStoreShowPurchaseUIResult", "XStoreShowRateAndReviewUIAsync",
      "XStoreShowRateAndReviewUIResult", "XStoreShowRedeemTokenUIAsync", "XStoreShowRedeemTokenUIResult", "XStoreQueryGameAndDlcPackageUpdatesAsync",
      "XStoreQueryGameAndDlcPackageUpdatesResultCount", "XStoreQueryGameAndDlcPackageUpdatesResult", "XStoreDownloadPackageUpdatesAsync",
      "XStoreDownloadPackageUpdatesResult", "XStoreDownloadAndInstallPackageUpdatesAsync", "XStoreDownloadAndInstallPackageUpdatesResult",
      "XStoreDownloadAndInstallPackagesAsync", "XStoreDownloadAndInstallPackagesResultCount", "XStoreDownloadAndInstallPackagesResult",
      "XStoreQueryPackageIdentifier", "XStoreRegisterGameLicenseChanged", "XStoreUnregisterGameLicenseChanged", "XStoreRegisterPackageLicenseLost",
      "XStoreUnregisterPackageLicenseLost"}},
    {"IXStoreImpl3",
     {"XStoreAcquireLicenseForDurablesAsync", "XStoreAcquireLicenseForDurablesResult"}},
    {"IXStoreImpl4",
     {"XStoreShowAssociatedProductsUIAsync", "XStoreShowAssociatedProductsUIResult", "XStoreShowProductPageUIAsync", "XStoreShowProductPageUIResult"}},
    {"IXStoreImpl5",
     {"XStoreQueryAssociatedProductsForStoreIdAsync", "XStoreQueryAssociatedProductsForStoreIdResult", "XStoreQueryPackageUpdatesAsync",
      "XStoreQueryPackageUpdatesResultCount", "XStoreQueryPackageUpdatesResult"}},
    {"IXStoreImpl6",
     {"XStoreShowGiftingUIAsync", "XStoreShowGiftingUIResult"}},
    {"IXSystemImpl",
     {"XSystemGetConsoleId", "XSystemGetXboxLiveSandboxId", "XSystemGetAppSpecificDeviceId"}},
    {"IXSystemImpl3",
     {"XSystemHandleTrack", "XSystemIsHandleValid"}},
    {"IXSystemImpl4",
     {"XSystemAllowFullDownloadBandwidth"}},
    {"IXSystemAnalyticsImpl",
     {"XSystemGetAnalyticsInfo"}},
    {"IXUserImpl",
     {"XUserDuplicateHandle", "XUserCloseHandle", "XUserCompare", "XUserGetMaxUsers", "XUserAddAsync", "XUserAddResult", "XUserGetLocalId",
      "XUserFindUserByLocalId", "XUserGetId", "XUserFindUserById", "XUserGetIsGuest", "XUserGetState", "XUserGetGamerPictureAsync",
      "XUserGetGamerPictureResultSize", "XUserGetGamerPictureResult", "XUserGetAgeGroup", "XUserCheckPrivilege", "XUserResolvePrivilegeWithUiAsync",
      "XUserResolvePrivilegeWithUiResult", "XUserGetTokenAndSignatureAsync", "XUserGetTokenAndSignatureResultSize",
      "XUserGetTokenAndSignatureResult", "XUserGetTokenAndSignatureUtf16Async", "XUserGetTokenAndSignatureUtf16ResultSize",
      "XUserGetTokenAndSignatureUtf16Result", "XUserResolveIssueWithUiAsync", "XUserResolveIssueWithUiResult", "XUserResolveIssueWithUiUtf16Async",
      "XUserResolveIssueWithUiUtf16Result", "XUserRegisterForChangeEvent", "XUserUnregisterForChangeEvent", "XUserGetSignOutDeferral",
      "XUserCloseSignOutDeferralHandle"}},
    {"IXUserImpl2",
     {"XUserAddByIdWithUiAsync", "XUserAddByIdWithUiResult"}},
    {"IXUserImpl3",
     {"XUserGetMsaTokenSilentlyAsync", "XUserGetMsaTokenSilentlyResult", "XUserGetMsaTokenSilentlyResultSize"}},
    {"IXUserImpl4",
     {"XUserIsStoreUser"}},
    {"IXUserImpl5",
     {"XUserPlatformRemoteConnectSetEventHandlers", "XUserPlatformRemoteConnectCancelPrompt", "XUserPlatformSpopPromptSetEventHandlers",
      "XUserPlatformSpopPromptComplete"}},
    {"IXUserImpl6",
     {"XUserIsSignOutPresent", "XUserSignOutAsync", "XUserSignOutResult"}},
    {"IXUserDeviceImpl",
     {"XUserFindForDevice", "XUserRegisterForDeviceAssociationChanged", "XUserUnregisterForDeviceAssociationChanged",
      "XUserGetDefaultAudioEndpointUtf16", "XUserRegisterForDefaultAudioEndpointUtf16Changed", "XUserUnregisterForDefaultAudioEndpointUtf16Changed",
      "XUserFindControllerForUserWithUiAsync", "XUserFindControllerForUserWithUiResult"}},
    {"IXUserGamertagImpl",
     {"XUserGetGamertag"}},
};

static const char* const kRenamedInTable[][2] = {{"XPackageGetPackageKind", "XPackageGetKind"}};
static const char* const kDocumentedButAbsent[] = {"XSystemGetDeviceType", "XSystemGetRuntimeInfo"};

static void CheckInterface(const Documented& d) {
  Block block = InterfaceBlock(d.iface);
  if (!block.found()) {
    Check(false, std::string(d.iface) + " is in table.json");
    return;
  }
  GUID iid{};
  if (!Uuid(block, &iid)) {
    Check(false, std::string(d.iface) + " has a uuid in table.json");
    return;
  }
  void* object = nullptr;
  HRESULT hr = g_query(iid, iid, &object);
  if (FAILED(hr) || !object) {
    Check(false, std::string(d.iface) + " QueryApiImpl " + std::to_string(static_cast<unsigned long>(hr)));
    return;
  }
  void** slots = *static_cast<void***>(object);
  int count = Slots(block);
  int filled = 0;
  for (const char* method : d.methods) {
    int slot = SlotIn(block, method);
    if (slot == -2) {
      Check(false, std::string(d.iface) + "::" + method + " appears twice in table.json");
    } else if (slot < 0) {
      Check(false, std::string(d.iface) + "::" + method + " is documented and has no slot in table.json");
    } else if (slot >= count) {
      Check(false, std::string(d.iface) + "::" + method + " slot " + std::to_string(slot) + " is past the interface's " + std::to_string(count) + " slots");
    } else if (!slots[slot]) {
      Check(false, std::string(d.iface) + "::" + method + " slot " + std::to_string(slot) + " is null in the DLL");
    } else {
      filled++;
    }
  }
  Check(filled == static_cast<int>(d.methods.size()),
        std::string(d.iface) + ": " + std::to_string(filled) + " of " + std::to_string(d.methods.size()) + " documented names resolve to filled slots");
}

int main() {
  HMODULE module = LoadLibraryA("xgameruntime.dll");
  if (!module) {
    std::printf("FAIL LoadLibrary\n");
    return 1;
  }
  g_query = reinterpret_cast<QueryApi>(GetProcAddress(module, "QueryApiImpl"));
  if (!g_query) {
    std::printf("FAIL QueryApiImpl export\n");
    return 1;
  }
  if (!LoadTable()) {
    std::printf("FAIL table.json not found next to the test (run it from the build directory)\n");
    return 1;
  }
  size_t names = 0;
  for (const Documented& d : kDocumented) {
    CheckInterface(d);
    names += d.methods.size();
  }
  for (const auto& pair : kRenamedInTable) {
    Check(!NamedAnywhere(pair[0]) && NamedAnywhere(pair[1]),
          std::string("the table names the documented ") + pair[0] + " " + pair[1]);
  }
  for (const char* method : kDocumentedButAbsent) {
    Check(!NamedAnywhere(method), std::string(method) + " is documented and still absent from the table (remove it from this list when it is added)");
  }
  std::printf("%s: %zu documented names in %zu interfaces, %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", names,
              sizeof(kDocumented) / sizeof(kDocumented[0]), g_failures);
  return g_failures ? 1 : 0;
}
