// MIT License
//
// Copyright(c) 2026 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "Tracing.h"
#include "commit.h"

namespace {
    struct EyeTrackerNotSupportedException : public std::exception {
        const char* what() const throw() {
            return "Eye tracker is not supported";
        }
    };

    std::unique_ptr<vr::IServerTrackedDeviceProvider> thisDriver;

    struct Driver : public vr::IServerTrackedDeviceProvider {
      public:
        Driver() {
        }

        virtual ~Driver() {
            Cleanup();
        };

        vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_Init");

            VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);

            if (!m_varjoSession) {
                DriverLog("Driver version: %s", DriverCommitHash);

                try {
                    if (!varjo_IsAvailable()) {
                        TraceLoggingWriteTagged(local, "Driver_Init_VarjoIsAvailableError");
                        throw EyeTrackerNotSupportedException();
                    }

                    m_varjoSession = varjo_SessionInit();
                    if (!m_varjoSession) {
                        TraceLoggingWriteTagged(local, "Driver_Init_VarjoCreateError");
                        throw EyeTrackerNotSupportedException();
                    }
                } catch (EyeTrackerNotSupportedException& exc) {
                    DriverLog("Failed to load driver: %s", exc.what());
                }
            }

            TraceLoggingWriteStop(local, "Driver_Init");

            return m_varjoSession ? vr::VRInitError_None : vr::VRInitError_Init_HmdNotFound;
        }

        void Cleanup() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_Cleanup");

            VR_CLEANUP_SERVER_DRIVER_CONTEXT();

            if (m_active.exchange(false)) {
                m_updateThread.join();
            }

            if (m_varjoSession) {
                varjo_SessionShutDown(m_varjoSession);
            }

            TraceLoggingWriteStop(local, "Driver_Cleanup");
        }

        const char* const* GetInterfaceVersions() override {
            return vr::k_InterfaceVersions;
        }

        void RunFrame() override {
            if (!m_varjoSession) {
                return;
            }

            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_RunFrame", TLArg(m_active.load(), "ThreadActive"));

            // Already running in background?
            if (m_active) {
                return;
            }

            vr::VREvent_t event;
            while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
                switch (event.eventType) {
                case vr::VREvent_TrackedDeviceActivated:
                    if (event.trackedDeviceIndex == vr::k_unTrackedDeviceIndex_Hmd) {
                        DriverLog("HMD Ready!");

                        // Schedule updates in a background thread.
                        m_active = true;
                        m_updateThread = std::thread(&Driver::UpdateThread, this);
                    }
                    break;
                }
            }

            TraceLoggingWriteStop(local, "Driver_RunFrame");
        }

        bool ShouldBlockStandbyMode() override {
            return false;
        }

        void EnterStandby() override {};

        void LeaveStandby() override {};

        void UpdateThread() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_UpdateThread");

            DriverLog("Hello from Driver::UpdateThread");
            SetThreadDescription(GetCurrentThread(), L"Driver_UpdateThread");

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(vr::k_unTrackedDeviceIndex_Hmd);

            // Advertise supportsEyeGazeInteraction.
            vr::VRProperties()->SetBoolProperty(container, vr::Prop_SupportsXrEyeGazeInteraction_Bool, true);

            // Create the input component for the eye gaze. It must have the path /eyetracking and nothing else!
            vr::VRInputComponentHandle_t eyeTrackingComponent = vr::k_ulInvalidInputComponentHandle;
            vr::VRDriverInput()->CreateEyeTrackingComponent(container, "/eyetracking", &eyeTrackingComponent);
            TraceLoggingWriteTagged(local, "Driver_UpdateThread", TLArg(eyeTrackingComponent, "EyeTrackingComponent"));
            DriverLog("Eye Gaze Component: %lld", eyeTrackingComponent);

            // Start getting gaze info for our session.
            varjo_GazeInit(m_varjoSession);

            vr::VREyeTrackingData_t data{};
            while (true) {
                // Wait for the next time to update.
                {
                    TraceLocalActivity(sleep);
                    TraceLoggingWriteStart(sleep, "Driver_UpdateThread_Sleep");

                    // We refresh the data at this frequency.
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                    TraceLoggingWriteStop(sleep, "Driver_UpdateThread_Sleep", TLArg(m_active.load(), "Active"));

                    if (!m_active) {
                        break;
                    }
                }

                // Retrieve the data from the eye tracker and push it to the input component.
                const auto gaze = varjo_GetGaze(m_varjoSession);
                TraceLoggingWriteTagged(local,
                                        "Driver_UpdateThread_VarjoGaze",
                                        TLArg((int)gaze.leftStatus, "LeftStatus"),
                                        TLArg((int)gaze.rightStatus, "RightStatus"));

                const bool isEyeTrackingDataAvailable =
                    gaze.leftStatus != varjo_GazeEyeStatus_Invalid && gaze.rightStatus != varjo_GazeEyeStatus_Invalid;
                if (isEyeTrackingDataAvailable) {
                    TraceLoggingWriteTagged(local,
                                            "Driver_UpdateThread_VarjoGaze",
                                            TLArg((float)gaze.leftEye.forward[0], "LeftGazeX"),
                                            TLArg((float)gaze.leftEye.forward[1], "LeftGazeY"),
                                            TLArg((float)gaze.leftEye.forward[2], "LeftGazeZ"),
                                            TLArg((float)gaze.rightEye.forward[0], "RightGazeX"),
                                            TLArg((float)gaze.rightEye.forward[1], "RightGazeY"),
                                            TLArg((float)gaze.rightEye.forward[2], "RightGazeZ"));

                    // Averaging both vectors.
                    data.vGazeTarget.v[0] = (float)(gaze.leftEye.forward[0] + gaze.rightEye.forward[0]) / 2.f;
                    data.vGazeTarget.v[1] = (float)(gaze.leftEye.forward[1] + gaze.rightEye.forward[1]) / 2.f;
                    data.vGazeTarget.v[2] = -(float)(gaze.leftEye.forward[2] + gaze.rightEye.forward[2]) / 2.f;
                    data.bValid = data.bTracked = data.bActive = true;
                } else {
                    // Fallback to identity.
                    data.vGazeTarget.v[0] = data.vGazeTarget.v[1] = 0;
                    data.vGazeTarget.v[1] = -1;
                    data.bValid = data.bTracked = data.bActive = false;
                }
                vr::VRDriverInput()->UpdateEyeTrackingComponent(eyeTrackingComponent, &data, 0.f);
            }

            DriverLog("Bye from Driver::UpdateThread");

            TraceLoggingWriteStop(local, "Driver_UpdateThread");
        }

        varjo_Session* m_varjoSession = nullptr;
        std::atomic<bool> m_active = false;
        std::thread m_updateThread;
    };
} // namespace

// Entry point for vrserver.
extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode) {
    if (strcmp(vr::IServerTrackedDeviceProvider_Version, pInterfaceName) == 0) {
        if (!thisDriver) {
            thisDriver = std::make_unique<Driver>();
        }
        return thisDriver.get();
    }
    if (pReturnCode) {
        *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
    }
    return nullptr;
}
