#include "cbase.h"
#include <stdio.h>
#include <d3d9.h>
#include <D3D11.h>
#include <Windows.h>
#include <openvr.h>
#include <MinHook.h>
#include <convar.h>
#include <synchapi.h>
#include <processthreadsapi.h>
#include <isourcevirtualreality.h>
#include <imaterialsystem.h>
#include <cdll_client_int.h>
#include <thread>
#include <VRMod.h>


#pragma comment (lib, "d3d11.lib")
#pragma comment (lib, "d3d9.lib")



/*
//*************************************************************************
//  Current issues:		-  Frames sent to the HMD work perfectly fine on 720p but when we try to use the recommended HMD resolutions,
						   the HMD frames become black with part of a white rectangle off-bounds.
						THINGS I TRIED: 
							1. This issue is the same wether we Use Virtual Desktop or Oculus Link
							2. I tried overriding the framebuffer resolution successfully (via g_pMaterialSystem->SetRenderTargetFrameBufferSizeOverrides(recommendedWidth, recommendedHeight)
							and same for the "materials" variable), frames still didn't work.
							3. I added debugmessages. RenderTargetSize is indeed set correctly to the right resolution, frames still didn't work.
							4. If i try calling g_pMaterialSystem->EndRenderTargetAllocation(), frames are completely black, even for 720p
							5. I tried a boatload of flags for CreateNamedRenderTargetTextureEx. None of them fixed the problem.
							Some are interesting though. Disabling the depth buffer for example seems to submit frames to the HMD even if not 720p,
							but resolution still seems to be 720p and of course there's no depth anymore making you see through walls and stuff.

						POSSIBLE SOLUTIONS:
							Look inside void CViewRender::Render( vrect_t *rect ) in view.cpp
							it also contains the interesting function void CViewRender::SetUpViews()
							The following functions from pRenderContext may also be very interesting for solving this problem:
								// Sets/gets the viewport
							virtual void				Viewport( int x, int y, int width, int height ) = 0;
							virtual void				GetViewport( int& x, int& y, int& width, int& height ) const = 0;
								// Gets the window size
							virtual void GetWindowSize( int &width, int &height ) const = 0;
								// Sets the override sizes for all render target size tests. These replace the frame buffer size.

							// Set them when you are rendering primarily to something larger than the frame buffer (as in VR mode).
							virtual void				SetRenderTargetFrameBufferSizeOverrides( int nWidth, int nHeight ) = 0;

							// Returns the (possibly overridden) framebuffer size for render target sizing.
							virtual void				GetRenderTargetFrameBufferDimensions( int & nWidth, int & nHeight ) = 0;


						abstract_class IClientRenderable van iclientrenderable.h can also be very interesting. Especially the following functions:
							virtual bool					UsesPowerOfTwoFrameBufferTexture() = 0;
							virtual bool					UsesFullFrameBufferTexture() = 0;




						- When we compile the game for Release and then launch it via steam, the Source SDK 2013 Multiplayer
						  resets it's openvr.dll to it's previous way lighter version that gives us errors.


						- Trying to load Gravelpit crashes the game for some reason. Error is in tf_mapinfomenu.cpp






	Fixed issues:		- We get up to ShareTextureFinish. That one crashes the game and gives us: OpenSharedResource failedException thrown: read access violation.
						  **res** was nullptr.

						FIX:
						  - first you call ShareTextureBegin()
						  - then you CREATE a rendertarget texture (via CreateRenderTargetTexture(arguments) from IMaterialSystem class from imaterialsystem.h)
						  - then you call ShareTextureFinish() immediately after

						  Link that pointed us at a working solution:
						  https://developer.valvesoftware.com/wiki/Adding_a_Dynamic_Scope

						  After that, CreateTextureHook() got called A LOT of times, we still didn't get it working.
						  To fix that all we had to do was call g_pMaterialSystem->BeginRenderTargetAllocation(); right before creating the new RenderTarget.
	
	


						- Code won't compile because of errors with CreateThread and WaitForSingleObject in VRMOD_Sharetexturebegin(). I'm sure this has to do with the Windows Kit include files. Maybe it'll go away if we include more Windows Kit directories
						  Maybe it's a problem unique to Windows 10

						  POSSIBLE SOLUTIONS:  - add #include <Windows.h> to cbase.h and solve the compile errors we get after that.
						  - nuttige link:   https://social.msdn.microsoft.com/Forums/en-US/400c5cce-d6ed-4157-908e-41d5beb1ce13/link-error-2019-missing-definitions-from-windowsh?forum=vcgeneral

						FIX: Turns out there's a file named "protected_things.h". It redefined CreateThread to CreateThread__USE_VCR_MODE and WaitForSingleObject to WaitForSingleObject__USE_VCR_MODE
							 because some parameter was defined or something that enables extra valve safety precautions. Commenting these 2 redefinitions away fixed the problem.
							 I hope this won't come back to bite me in the ass later.
	
	
						- ConCommand vrmod_init("vrmod_init", VRMOD_Init, "Starts VRMod and SteamVR.") Won't work for some fucking reason even though it fucking should

						FIX: ConCommand only works on Void functions!

						- Latest error i keep getting is related to the code being Read only for some fucking reason. This is the same on my whole fucking harddrive and it keeps fucking resetting to Read only everytime i change it
						  FUCK WHATEVER IS RESPONSIBLE FOR THIS BULLSHIT. MIGHT BE FUCKING GIT THAT'S DOING THIS SHIT
						  UPDATE: turns out the folders aren't actually Read-only, they just seem like it. Maybe the shitty compiler doesn't know that though.

						FIX: Turns out there wasn't any read-only problem. The compile errors were caused by the change in directory structure due to moving and cloning the project with git.
							 Fixed this by deleting the project files and regenerating them with createtfmod.bat
//*************************************************************************
*/


//*************************************************************************
//  Globals
//*************************************************************************

#define MAX_STR_LEN 256
#define MAX_ACTIONS 64
#define MAX_ACTIONSETS 16

//openvr related
typedef struct {
    vr::VRActionHandle_t handle;
    char fullname[MAX_STR_LEN];
    char type[MAX_STR_LEN];
    char* name;
}action;

typedef struct {
    vr::VRActionSetHandle_t handle;
    char name[MAX_STR_LEN];
}actionSet;

vr::IVRSystem*          g_pSystem = NULL;
vr::IVRInput*           g_pInput = NULL;
vr::TrackedDevicePose_t g_poses[vr::k_unMaxTrackedDeviceCount];
actionSet               g_actionSets[MAX_ACTIONSETS];
int                     g_actionSetCount = 0;
vr::VRActiveActionSet_t g_activeActionSets[MAX_ACTIONSETS];
int                     g_activeActionSetCount = 0;
action                  g_actions[MAX_ACTIONS];
int                     g_actionCount = 0;

//directx
typedef HRESULT(APIENTRY* CreateTexture) (IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
CreateTexture           g_CreateTextureOriginal = NULL;
ID3D11Device*           g_d3d11Device = NULL;
ID3D11Texture2D*        g_d3d11Texture = NULL;
HANDLE                  g_sharedTexture = NULL;
DWORD_PTR               g_CreateTextureAddr = NULL;
IDirect3DDevice9*       g_d3d9Device = NULL;

//other
float                   g_horizontalOffsetLeft = 0;
float                   g_horizontalOffsetRight = 0;
float                   g_verticalOffsetLeft = 0;
float                   g_verticalOffsetRight = 0;
uint32_t recommendedWidth = 960;
uint32_t recommendedHeight = 1080;

// Extra Virtual Fortress 2 globals
int Virtual_Fortress_2_version = 1;



//*************************************************************************
//  CreateTexture hook																			// converted properly for Virtual Fortress 2 now.
//*************************************************************************
HRESULT APIENTRY CreateTextureHook(IDirect3DDevice9* pDevice, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** tex, HANDLE* shared_handle) {
		if (g_sharedTexture == NULL) {
			shared_handle = &g_sharedTexture;
			pool = D3DPOOL_DEFAULT;
			g_d3d9Device = pDevice;
		}
		return g_CreateTextureOriginal(pDevice, w, h, levels, usage, format, pool, tex, shared_handle);
};

//*************************************************************************
//    FindCreateTexture thread																	// converted properly for Virtual Fortress 2 now.
//*************************************************************************
DWORD WINAPI FindCreateTexture(LPVOID lParam) {
    IDirect3D9* dx = Direct3DCreate9(D3D_SDK_VERSION);
    if (dx == NULL) {
        return 1;
    }

    HWND window = CreateWindowA("BUTTON", " ", WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (window == NULL) {
        dx->Release();
        return 2;
    }

    IDirect3DDevice9* d3d9Device = NULL;

    D3DPRESENT_PARAMETERS params;
    ZeroMemory(&params, sizeof(params));
    params.Windowed = TRUE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.BackBufferFormat = D3DFMT_UNKNOWN;

    //calling CreateDevice on the main thread seems to start causing random lua errors
    if (dx->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &params, &d3d9Device) != D3D_OK) {
        dx->Release();
        DestroyWindow(window);
        return 3;
    }

    g_CreateTextureAddr = ((DWORD_PTR*)(((DWORD_PTR*)d3d9Device)[0]))[23];

    d3d9Device->Release();
    dx->Release();
    DestroyWindow(window);

    return 0;
}

//*************************************************************************
//    Lua function: VRMOD_GetVersion()
//    Returns: number
//*************************************************************************
void VRMOD_GetVersion() {
	Msg("Current Virtual Fortress 2 version is : %i", Virtual_Fortress_2_version);
    return;
}

ConCommand vrmod_getversion("vrmod_getversion", VRMOD_GetVersion, "Returns the current version of the Virtual Fortress 2 mod.");

//*************************************************************************
//    VRMOD_Init():		Initialize SteamVR and set some important globals						// converted properly for Virtual Fortress 2 now.
//*************************************************************************
 void VRMOD_Init() {
    vr::HmdError error = vr::VRInitError_None;

    g_pSystem = vr::VR_Init(&error, vr::VRApplication_Scene);
    if (error != vr::VRInitError_None) {
        Warning("VR_Init failed");
    }

    if (!vr::VRCompositor()) {
        Warning("VRCompositor failed");
    }

    vr::HmdMatrix44_t proj = g_pSystem->GetProjectionMatrix(vr::Hmd_Eye::Eye_Left, 1, 10);
    float xscale = proj.m[0][0];
    float xoffset = proj.m[0][2];
    float yscale = proj.m[1][1];
    float yoffset = proj.m[1][2];
    float tan_px = fabsf((1.0f - xoffset) / xscale);
    float tan_nx = fabsf((-1.0f - xoffset) / xscale);
    float tan_py = fabsf((1.0f - yoffset) / yscale);
    float tan_ny = fabsf((-1.0f - yoffset) / yscale);
    float w = tan_px + tan_nx;
    float h = tan_py + tan_ny;
    g_horizontalFOVLeft = atan(w / 2.0f) * 180 / 3.141592654 * 2;
    //g_verticalFOV = atan(h / 2.0f) * 180 / 3.141592654 * 2;
    g_aspectRatioLeft = w / h;
    g_horizontalOffsetLeft = xoffset;
    g_verticalOffsetLeft = yoffset;

    proj = g_pSystem->GetProjectionMatrix(vr::Hmd_Eye::Eye_Right, 1, 10);
    xscale = proj.m[0][0];
    xoffset = proj.m[0][2];
    yscale = proj.m[1][1];
    yoffset = proj.m[1][2];
    tan_px = fabsf((1.0f - xoffset) / xscale);
    tan_nx = fabsf((-1.0f - xoffset) / xscale);
    tan_py = fabsf((1.0f - yoffset) / yscale);
    tan_ny = fabsf((-1.0f - yoffset) / yscale);
    w = tan_px + tan_nx;
    h = tan_py + tan_ny;
    g_horizontalFOVRight = atan(w / 2.0f) * 180 / 3.141592654 * 2;
    g_aspectRatioRight = w / h;
    g_horizontalOffsetRight = xoffset;
    g_verticalOffsetRight = yoffset;

    return;
 }

 ConCommand vrmod_init("vrmod_init", VRMOD_Init, "Starts VRMod and SteamVR.");

//*************************************************************************
//    VRMOD_SetActionManifest(fileName)													// Probably converted properly for Virtual Fortress 2 now.
//*************************************************************************
int VRMOD_SetActionManifest(const char* fileName) {

    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\tf_port\\%s", currentDir, fileName);

    g_pInput = vr::VRInput();
    if (g_pInput->SetActionManifestPath(path) != vr::VRInputError_None) {
		Warning("SetActionManifestPath failed");
    }

    FILE* file = NULL;
    fopen_s(&file, path, "r");
    if (file == NULL) {
		Warning("failed to open action manifest");
    }

    memset(g_actions, 0, sizeof(g_actions));

    char word[MAX_STR_LEN];
    while (fscanf_s(file, "%*[^\"]\"%[^\"]\"", word, MAX_STR_LEN) == 1 && strcmp(word, "actions") != 0);
    while (fscanf_s(file, "%[^\"]\"", word, MAX_STR_LEN) == 1) {
        if (strchr(word, ']') != nullptr)
            break;
        if (strcmp(word, "name") == 0) {
            if (fscanf_s(file, "%*[^\"]\"%[^\"]\"", g_actions[g_actionCount].fullname, MAX_STR_LEN) != 1)
                break;
            g_actions[g_actionCount].name = g_actions[g_actionCount].fullname;
            for (int i = 0; i < strlen(g_actions[g_actionCount].fullname); i++) {
                if (g_actions[g_actionCount].fullname[i] == '/')
                    g_actions[g_actionCount].name = g_actions[g_actionCount].fullname + i + 1;
            }
            g_pInput->GetActionHandle(g_actions[g_actionCount].fullname, &(g_actions[g_actionCount].handle));
        }
        if (strcmp(word, "type") == 0) {
            if (fscanf_s(file, "%*[^\"]\"%[^\"]\"", g_actions[g_actionCount].type, MAX_STR_LEN) != 1)
                break;
        }
        if (g_actions[g_actionCount].fullname[0] && g_actions[g_actionCount].type[0]) {
            g_actionCount++;
            if (g_actionCount == MAX_ACTIONS)
                break;
        }
    }

    fclose(file);

    return 0;
}

//*************************************************************************
//    Lua function: VRMOD_SetActiveActionSets(name, ...)
//*************************************************************************
void VRMOD_SetActiveActionSets(const char* actionSetNames [MAX_ACTIONSETS]) {		// We might have converted this incorrectly from LUA to C++
    g_activeActionSetCount = 0;
    for (int i = 0; i < MAX_ACTIONSETS; i++) {
        //if (LUA->GetType(i + 1) == GarrysMod::Lua::Type::STRING) {
            //const char* actionSetName = LUA->CheckString(i + 1);
		const char* actionSetName = actionSetNames[i + 1];
            int actionSetIndex = -1;
            for (int j = 0; j < g_actionSetCount; j++) {
                if (strcmp(actionSetName, g_actionSets[j].name) == 0) {
                    actionSetIndex = j;
                    break;
                }
            }
            if (actionSetIndex == -1) {
                g_pInput->GetActionSetHandle(actionSetName, &g_actionSets[g_actionSetCount].handle);
                memcpy(g_actionSets[g_actionSetCount].name, actionSetName, strlen(actionSetName));
                actionSetIndex = g_actionSetCount;
                g_actionSetCount++;
            }
            g_activeActionSets[g_activeActionSetCount].ulActionSet = g_actionSets[actionSetIndex].handle;
            g_activeActionSetCount++;
        //}
        //else {
        //    break;
        //}
    }
	return;
    //return 0;
}

//*************************************************************************
//    Lua function: VRMOD_GetViewParameters()											// IMPORTANT FOR HEADTRACKING !!! Properly adjusted for Virtual Fortress 2 now.
//    Returns: table
//*************************************************************************
void VRMOD_GetViewParameters(Vector &eyeToHeadTransformPosLeft, Vector &eyeToHeadTransformPosRight) {

    //uint32_t recommendedWidth = 0;
    //uint32_t recommendedHeight = 0;
    g_pSystem->GetRecommendedRenderTargetSize(&recommendedWidth, &recommendedHeight);

    vr::HmdMatrix34_t eyeToHeadLeft = g_pSystem->GetEyeToHeadTransform(vr::Eye_Left);
    vr::HmdMatrix34_t eyeToHeadRight = g_pSystem->GetEyeToHeadTransform(vr::Eye_Right);
    eyeToHeadTransformPosLeft.x = eyeToHeadLeft.m[0][3];
    eyeToHeadTransformPosLeft.y = eyeToHeadLeft.m[1][3];
    eyeToHeadTransformPosLeft.z = eyeToHeadLeft.m[2][3];

    eyeToHeadTransformPosRight.x = eyeToHeadRight.m[0][3];
    eyeToHeadTransformPosRight.y = eyeToHeadRight.m[1][3];
    eyeToHeadTransformPosRight.z = eyeToHeadRight.m[2][3];

	return;
}

//*************************************************************************
//    Lua function: VRMOD_UpdatePosesAndActions()
//*************************************************************************
void VRMOD_UpdatePosesAndActions() {
    vr::VRCompositor()->WaitGetPoses(g_poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
   // g_pInput->UpdateActionState(g_activeActionSets, sizeof(vr::VRActiveActionSet_t), g_activeActionSetCount);		// UNCOMMENT THIS ONCE WE'VE GOT THE ACTION FUNCTIONS WORKING

	return;
}



//*************************************************************************
//    Lua function: VRMOD_GetPoses()														// IMPORTANT FOR HEADTRACKING !!!
//    Returns: table
//*************************************************************************
void VRMOD_GetPoses(Vector &pos, Vector &vel, QAngle &ang, QAngle &angvel) {	// To accomodate this function's code properly maybe we need to pass (dynamic length?) arrays of these arguments?
    vr::InputPoseActionData_t poseActionData;									// NO! We actually need to make global dynamic length array so C won't complain!
    vr::TrackedDevicePose_t pose;
    char poseName[MAX_STR_LEN];

    //LUA->CreateTable();

    for (int i = -1; i < g_actionCount; i++) {
        //select a pose
        poseActionData.pose.bPoseIsValid = 0;
        pose.bPoseIsValid = 0;
        if (i == -1) {
            pose = g_poses[0];
            memcpy(poseName, "hmd", 4);
        }
        else if (strcmp(g_actions[i].type, "pose") == 0) {
            //g_pInput->GetPoseActionData(g_actions[i].handle, vr::TrackingUniverseStanding, 0, &poseActionData, sizeof(poseActionData), vr::k_ulInvalidInputValueHandle);
			g_pInput->GetPoseActionDataRelativeToNow(g_actions[i].handle, vr::TrackingUniverseStanding, 0, &poseActionData, sizeof(poseActionData), vr::k_ulInvalidInputValueHandle);
            pose = poseActionData.pose;
            strcpy_s(poseName, MAX_STR_LEN, g_actions[i].name);
        }
        else {
            continue;
        }
        
        if (pose.bPoseIsValid) {

            vr::HmdMatrix34_t mat = pose.mDeviceToAbsoluteTracking;
            //Vector pos;
            //Vector vel;
            //QAngle ang;
            QAngle angvel;
            pos.x = -mat.m[2][3];
            pos.y = -mat.m[0][3];
            pos.z = mat.m[1][3];
            ang.x = asin(mat.m[1][2]) * (180.0 / 3.141592654);
            ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0 / 3.141592654);
            ang.z = atan2f(-mat.m[1][0], mat.m[1][1]) * (180.0 / 3.141592654);
            vel.x = -pose.vVelocity.v[2];
            vel.y = -pose.vVelocity.v[0];
            vel.z = pose.vVelocity.v[1];
            angvel.x = -pose.vAngularVelocity.v[2] * (180.0 / 3.141592654);
            angvel.y = -pose.vAngularVelocity.v[0] * (180.0 / 3.141592654);
            angvel.z = pose.vAngularVelocity.v[1] * (180.0 / 3.141592654);

            /*LUA->CreateTable();

            LUA->PushVector(pos);
            LUA->SetField(-2, "pos");

            LUA->PushVector(vel);
            LUA->SetField(-2, "vel");

            LUA->PushAngle(ang);
            LUA->SetField(-2, "ang");

            LUA->PushAngle(angvel);
            LUA->SetField(-2, "angvel");

            LUA->SetField(-2, poseName);
			*/
        }
    }

	return;
    //return 1;
}

////*************************************************************************
////    Lua function: VRMOD_GetActions()
////    Returns: table
////*************************************************************************
void VRMOD_GetActions() {
    vr::InputDigitalActionData_t digitalActionData;
    vr::InputAnalogActionData_t analogActionData;
    vr::VRSkeletalSummaryData_t skeletalSummaryData;

    //LUA->CreateTable();

    for (int i = 0; i < g_actionCount; i++) {
        if (strcmp(g_actions[i].type, "boolean") == 0) {
            //LUA->PushBool((g_pInput->GetDigitalActionData(g_actions[i].handle, &digitalActionData, sizeof(digitalActionData), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && digitalActionData.bState));
			bool bool1 = (g_pInput->GetDigitalActionData(g_actions[i].handle, &digitalActionData, sizeof(digitalActionData), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None && digitalActionData.bState);
            //LUA->SetField(-2, g_actions[i].name);
        }
        else if (strcmp(g_actions[i].type, "vector1") == 0) {
            g_pInput->GetAnalogActionData(g_actions[i].handle, &analogActionData, sizeof(analogActionData), vr::k_ulInvalidInputValueHandle);
			float float1 = analogActionData.x;
            //LUA->PushNumber(analogActionData.x);
            //LUA->SetField(-2, g_actions[i].name);
        }
        else if (strcmp(g_actions[i].type, "vector2") == 0) {
            //LUA->CreateTable();
            g_pInput->GetAnalogActionData(g_actions[i].handle, &analogActionData, sizeof(analogActionData), vr::k_ulInvalidInputValueHandle);
			float float2 = analogActionData.x;
			float float3 = analogActionData.y;
            /*LUA->PushNumber(analogActionData.x);
            LUA->SetField(-2, "x");
            LUA->PushNumber(analogActionData.y);
            LUA->SetField(-2, "y");
            LUA->SetField(-2, g_actions[i].name);
			*/
        }
       // else if (strcmp(g_actions[i].type, "skeleton") == 0) {
        //    g_pInput->GetSkeletalSummaryData(g_actions[i].handle, &skeletalSummaryData);
            //LUA->CreateTable();
            //LUA->CreateTable();
            //for (int j = 0; j < 5; j++) {
                //LUA->PushNumber(j + 1);
                //LUA->PushNumber(skeletalSummaryData.flFingerCurl[j]);
                //LUA->SetTable(-3);
           // }
           // LUA->SetField(-2, "fingerCurls");
           // LUA->SetField(-2, g_actions[i].name);
        //}
    }

	return;
    //return 1;
}

//*************************************************************************
//    VRMOD_ShareTextureBegin()																// converted properly for Virtual Fortress 2 now.
//*************************************************************************
void VRMOD_ShareTextureBegin() {
    HWND activeWindow = GetActiveWindow();
    if (activeWindow == NULL) {
        Warning("GetActiveWindow failed");
    }

    //hiding and restoring the game window is a workaround to d3d9 CreateDevice
    //failing on the second thread if the game is fullscreen
    ShowWindow(activeWindow, SW_HIDE);
    HANDLE thread = CreateThread(NULL, 0, FindCreateTexture, 0, 0, NULL);
    if (thread == NULL) {
        Warning("CreateThread failed");
    }
    WaitForSingleObject(thread, 1000);  // Needs to be at least 27 milliseconds before we start seeing CreateTextureHook() being called
    ShowWindow(activeWindow, SW_RESTORE);
    DWORD exitCode = 4;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    if (exitCode != 0) {
        if (exitCode == 1) {
			Warning("Direct3DCreate9 failed");
        }
        else if (exitCode == 2) {
			Warning("CreateWindowA failed");
        }
        else if (exitCode == 3) {
			Warning("CreateDevice failed");
        }
        else {
			Warning("GetExitCodeThread failed");
        }
    }

    if (g_CreateTextureAddr == NULL) {
		Warning("g_CreateTextureAddr is null");
    }

    g_CreateTextureOriginal = (CreateTexture)g_CreateTextureAddr;

    if (MH_Initialize() != MH_OK) {
		Warning("MH_Initialize failed");
    }

    if (MH_CreateHook((DWORD_PTR*)g_CreateTextureAddr, &CreateTextureHook, reinterpret_cast<void**>(&g_CreateTextureOriginal)) != MH_OK) {
		Warning("MH_CreateHook failed");
    }

    if (MH_EnableHook((DWORD_PTR*)g_CreateTextureAddr) != MH_OK) {
		Warning("MH_EnableHook failed");
    }

    return;
}


//*************************************************************************
//    VRMOD_ShareTextureFinish()															// converted properly for Virtual Fortress 2 now.
//*************************************************************************
void VRMOD_ShareTextureFinish() {
    if (g_sharedTexture == NULL) {
		Warning("g_sharedTexture is null");
    }

    if (D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &g_d3d11Device, NULL, NULL) != S_OK) {
		Warning("D3D11CreateDevice failed");
    }

    ID3D11Resource* res;
    if (FAILED(g_d3d11Device->OpenSharedResource(g_sharedTexture, __uuidof(ID3D11Resource), (void**)&res))) {
		Warning("OpenSharedResource failed");
    }

    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&g_d3d11Texture))) {
		Warning("QueryInterface failed");
    }

    MH_DisableHook((DWORD_PTR*)g_CreateTextureAddr);
    MH_RemoveHook((DWORD_PTR*)g_CreateTextureAddr);
    if (MH_Uninitialize() != MH_OK) {
		Warning("MH_Uninitialize failed");
    }

    return;
}


//*************************************************************************
//    VRMOD_SubmitSharedTexture()																// converted properly for Virtual Fortress 2 now.
//*************************************************************************
void VRMOD_SubmitSharedTexture() {
    if (g_d3d11Texture == NULL)
        return;

    IDirect3DQuery9* pEventQuery = nullptr;
    g_d3d9Device->CreateQuery(D3DQUERYTYPE_EVENT, &pEventQuery);
    if (pEventQuery != nullptr)
    {
        pEventQuery->Issue(D3DISSUE_END);
        while (pEventQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) != S_OK);
        pEventQuery->Release();
    }

    vr::Texture_t vrTexture = { g_d3d11Texture, vr::TextureType_DirectX, vr::ColorSpace_Auto };

    vr::VRTextureBounds_t textureBounds;

    //submit Left eye
    textureBounds.uMin = 0.0f + g_horizontalOffsetLeft * 0.25f;
    textureBounds.uMax = 0.5f + g_horizontalOffsetLeft * 0.25f;
    textureBounds.vMin = 0.0f - g_verticalOffsetLeft * 0.5f;
    textureBounds.vMax = 1.0f - g_verticalOffsetLeft * 0.5f;

    vr::VRCompositor()->Submit(vr::EVREye::Eye_Left, &vrTexture, &textureBounds);

    //submit Right eye
    textureBounds.uMin = 0.5f + g_horizontalOffsetRight * 0.25f;
    textureBounds.uMax = 1.0f + g_horizontalOffsetRight * 0.25f;
    textureBounds.vMin = 0.0f - g_verticalOffsetRight * 0.5f;
    textureBounds.vMax = 1.0f - g_verticalOffsetRight * 0.5f;

    vr::VRCompositor()->Submit(vr::EVREye::Eye_Right, &vrTexture, &textureBounds);

    return;
}


//*************************************************************************
//    VRMOD_Start()																				// converted properly for Virtual Fortress 2 now.
//*************************************************************************
void VRMOD_Start() {

    g_pSystem->GetRecommendedRenderTargetSize(&recommendedWidth, &recommendedHeight);
	// Temporarily change resolution untill we can use the actual recommended resolution without messing up rendering
	recommendedWidth = 960;
	recommendedHeight = 1080;

	VRMOD_ShareTextureBegin();
	g_pMaterialSystem->BeginRenderTargetAllocation();
	RenderTarget_VRMod = g_pMaterialSystem->CreateNamedRenderTargetTextureEx("vrmod_rt", 2 * recommendedWidth, recommendedHeight, RT_SIZE_DEFAULT, g_pMaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
	VRMOD_ShareTextureFinish();
	VRMod_Started = 1;

}
ConCommand vrmod_start("vrmod_start", VRMOD_Start, "Finally starts VRMod");


//*************************************************************************
//    VRMOD_Shutdown()																			// converted properly for Virtual Fortress 2 now.
//*************************************************************************
void VRMOD_Shutdown() {
    if (g_pSystem != NULL) {
        vr::VR_Shutdown();
        g_pSystem = NULL;
    }
	VRMod_Started = 0;
	g_pMaterialSystem->EndRenderTargetAllocation();
    if (g_d3d11Device != NULL) {
        g_d3d11Device->Release();
        g_d3d11Device = NULL;
    }
    g_d3d11Texture = NULL;
    g_sharedTexture = NULL;
    g_CreateTextureAddr = NULL;
    g_actionCount = 0;
    g_actionSetCount = 0;
    g_activeActionSetCount = 0;
    g_d3d9Device = NULL;
	
    return;
}

ConCommand vrmod_shutdown("vrmod_shutdown", VRMOD_Shutdown, "Stops VRMod and SteamVR and cleans up.");


//*************************************************************************
//    Lua function: VRMOD_TriggerHaptic(actionName, delay, duration, frequency, amplitude)				// converted properly for Virtual Fortress 2 now.
//*************************************************************************
void VRMOD_TriggerHaptic(const char* actionName, float delay, float duration, float frequency, float amplitude) {
    unsigned int nameLen = strlen(actionName);
    for (int i = 0; i < g_actionCount; i++) {
        if (strlen(g_actions[i].name) == nameLen && memcmp(g_actions[i].name, actionName, nameLen) == 0) {
			g_pInput->TriggerHapticVibrationAction(g_actions[i].handle, delay, duration, frequency, amplitude, vr::k_ulInvalidInputValueHandle);
            break;
        }
    }
	return;
}

//*************************************************************************
//    Lua function: VRMOD_GetTrackedDeviceNames()
//    Returns: table
//*************************************************************************
void VRMOD_GetTrackedDeviceNames() {
    //LUA->CreateTable();
    int tableIndex = 1;
    char name[MAX_STR_LEN];
    for (int i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
        if (g_pSystem->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, name, MAX_STR_LEN) > 1) {
            //LUA->PushNumber(tableIndex);
            //LUA->PushString(name);
            //LUA->SetTable(-3);
            tableIndex++;
        }
    }
	return ;
    //return 1;
}


int VRMOD_GetRecWidth()																					// works properly for Virtual Fortress 2 now.
{
	g_pSystem->GetRecommendedRenderTargetSize(&recommendedWidth, &recommendedHeight);
	return recommendedWidth;
}

int VRMOD_GetRecHeight()																				// works properly for Virtual Fortress 2 now.
{
	g_pSystem->GetRecommendedRenderTargetSize(&recommendedWidth, &recommendedHeight);
	return recommendedHeight;
}