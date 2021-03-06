
#include "stdafx.h"
#include "driver_vrinputemulator.h"
#include <openvr_math.h>
#undef WIN32_LEAN_AND_MEAN
#undef NOSOUND
#include <Windows.h>
// According to windows documentation mmsystem.h should be automatically included with Windows.h when WIN32_LEAN_AND_MEAN and NOSOUND are not defined
// But it doesn't work so I have to include it manually
#include <mmsystem.h>


namespace vrinputemulator {
namespace driver {


#define VECTOR_ADD(lhs, rhs) \
		lhs[0] += rhs.v[0]; \
		lhs[1] += rhs.v[1]; \
		lhs[2] += rhs.v[2];


bool OpenvrDeviceManipulationInfo::touchpadEmulationEnabledFlag = true;


void OpenvrDeviceManipulationInfo::setDigitalInputRemapping(uint32_t buttonId, const DigitalInputRemapping& remapping) {
	if (remapping.valid) {
		m_digitalInputRemapping[buttonId].remapping = remapping;
	} else {
		auto it = m_digitalInputRemapping.find(buttonId);
		if (it != m_digitalInputRemapping.end()) {
			m_digitalInputRemapping.erase(it);
		}
	}
}


DigitalInputRemapping OpenvrDeviceManipulationInfo::getDigitalInputRemapping(uint32_t buttonId) {
	auto it = m_digitalInputRemapping.find(buttonId);
	if (it != m_digitalInputRemapping.end()) {
		return it->second.remapping;
	} else {
		return DigitalInputRemapping();
	}
}


void OpenvrDeviceManipulationInfo::setAnalogInputRemapping(uint32_t axisId, const AnalogInputRemapping& remapping) {
	if (axisId < 5) {
		m_analogInputRemapping[axisId].remapping = remapping;
	}
}


AnalogInputRemapping OpenvrDeviceManipulationInfo::getAnalogInputRemapping(uint32_t axisId) {
	if (axisId < 5) {
		return m_analogInputRemapping[axisId].remapping;
	} else {
		return AnalogInputRemapping();
	}
}


void OpenvrDeviceManipulationInfo::handleNewDevicePose(vr::IVRServerDriverHost* driver, _DetourTrackedDevicePoseUpdated_t origFunc, uint32_t& unWhichDevice, const vr::DriverPose_t& pose) {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	if (m_deviceMode == 1) { // fake disconnect mode
		if (!_disconnectedMsgSend) {
			vr::DriverPose_t newPose = pose;
			newPose.poseIsValid = false;
			newPose.deviceIsConnected = false;
			newPose.result = vr::TrackingResult_Uninitialized;
			_disconnectedMsgSend = true;
			origFunc(driver, unWhichDevice, newPose, sizeof(vr::DriverPose_t));
		}
	} else if (m_deviceMode == 3 && !m_redirectSuspended) { // redirect target
		//nop
	} else if (m_deviceMode == 5) { // motion compensation mode
		auto serverDriver = CServerDriver::getInstance();
		if (serverDriver) {
			if (pose.poseIsValid && pose.result == vr::TrackingResult_Running_OK) {
				serverDriver->_setMotionCompensationStatus(MotionCompensationStatus::Running);
				if (!serverDriver->_isMotionCompensationZeroPoseValid()) {
					serverDriver->_setMotionCompensationZeroPose(pose);
					serverDriver->sendReplySetMotionCompensationMode(true);
				} else {
					serverDriver->_updateMotionCompensationRefPose(pose);
				}
			} else {
				if (!serverDriver->_isMotionCompensationZeroPoseValid()) {
					setDefaultMode();
					serverDriver->sendReplySetMotionCompensationMode(false);
				} else {
					serverDriver->_setMotionCompensationStatus(MotionCompensationStatus::MotionRefNotTracking);
				}
			}
		}
	} else {
		vr::DriverPose_t newPose = pose;
		if (m_offsetsEnabled) {
			if (m_worldFromDriverRotationOffset.w != 1.0 || m_worldFromDriverRotationOffset.x != 0.0
					|| m_worldFromDriverRotationOffset.y != 0.0 || m_worldFromDriverRotationOffset.z != 0.0) {
				newPose.qWorldFromDriverRotation = m_worldFromDriverRotationOffset * newPose.qWorldFromDriverRotation;
			}
			if (m_worldFromDriverTranslationOffset.v[0] != 0.0 || m_worldFromDriverTranslationOffset.v[1] != 0.0 || m_worldFromDriverTranslationOffset.v[2] != 0.0) {
				VECTOR_ADD(newPose.vecWorldFromDriverTranslation, m_worldFromDriverTranslationOffset);
			}
			if (m_driverFromHeadRotationOffset.w != 1.0 || m_driverFromHeadRotationOffset.x != 0.0
					|| m_driverFromHeadRotationOffset.y != 0.0 || m_driverFromHeadRotationOffset.z != 0.0) {
				newPose.qDriverFromHeadRotation = m_driverFromHeadRotationOffset * newPose.qDriverFromHeadRotation;
			}
			if (m_driverFromHeadTranslationOffset.v[0] != 0.0 || m_driverFromHeadTranslationOffset.v[1] != 0.0 || m_driverFromHeadTranslationOffset.v[2] != 0.0) {
				VECTOR_ADD(newPose.vecDriverFromHeadTranslation, m_driverFromHeadTranslationOffset);
			}
			if (m_deviceRotationOffset.w != 1.0 || m_deviceRotationOffset.x != 0.0
					|| m_deviceRotationOffset.y != 0.0 || m_deviceRotationOffset.z != 0.0) {
				newPose.qRotation = m_deviceRotationOffset * newPose.qRotation;
			}
			if (m_deviceTranslationOffset.v[0] != 0.0 || m_deviceTranslationOffset.v[1] != 0.0 || m_deviceTranslationOffset.v[2] != 0.0) {
				VECTOR_ADD(newPose.vecPosition, m_deviceTranslationOffset);
			}
		}
		auto serverDriver = CServerDriver::getInstance();
		if (serverDriver) {
			if (!serverDriver->_applyMotionCompensation(newPose, this)) {
				return;
			}
		}
		if (m_deviceMode == 2 && !m_redirectSuspended) { // redirect source
			if (!_disconnectedMsgSend) {
				vr::DriverPose_t newPose2 = pose;
				newPose2.poseIsValid = false;
				newPose2.deviceIsConnected = false;
				newPose2.result = vr::TrackingResult_Uninitialized;
				_disconnectedMsgSend = true;
				origFunc(driver, unWhichDevice, newPose2, sizeof(vr::DriverPose_t));
			}
			origFunc(m_redirectRef->driverHost(), m_redirectRef->openvrId(), newPose, sizeof(vr::DriverPose_t));
		} else if (m_deviceMode == 4) { // swap mode
			origFunc(m_redirectRef->driverHost(), m_redirectRef->openvrId(), newPose, sizeof(vr::DriverPose_t));
		} else {
			origFunc(driver, unWhichDevice, newPose, sizeof(vr::DriverPose_t));
		}
	}
}


void OpenvrDeviceManipulationInfo::handleButtonEvent(vr::IVRServerDriverHost* driver, uint32_t& unWhichDevice, ButtonEventType eventType, vr::EVRButtonId eButtonId, double eventTimeOffset) {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	auto buttonIt = m_digitalInputRemapping.find(eButtonId);
	if (buttonIt != m_digitalInputRemapping.end()) {
		auto& buttonInfo = buttonIt->second;
		if (buttonInfo.remapping.valid) {
			if (buttonInfo.remapping.touchAsClick) {
				switch (eventType) {
					case vrinputemulator::ButtonEventType::ButtonPressed:
					case vrinputemulator::ButtonEventType::ButtonUnpressed:
						return;
					case vrinputemulator::ButtonEventType::ButtonTouched: {
						eventType = vrinputemulator::ButtonEventType::ButtonPressed;
					} break;
					case vrinputemulator::ButtonEventType::ButtonUntouched: {
						eventType = vrinputemulator::ButtonEventType::ButtonUnpressed;
					} break;
					default: {
					} break;
				}
			}
			if (eventType == ButtonEventType::ButtonTouched || eventType == ButtonEventType::ButtonUntouched) {
				if (!buttonInfo.remapping.doublePressEnabled && !buttonInfo.remapping.longPressEnabled) {
					sendDigitalBinding(buttonInfo.remapping.binding, unWhichDevice, eventType, eButtonId, eventTimeOffset, &buttonInfo.bindings[0]);
				}
			} else if (eventType == ButtonEventType::ButtonPressed || eventType == ButtonEventType::ButtonUnpressed) {
				switch (buttonInfo.state) {
				case 0: {
					if (!buttonInfo.remapping.doublePressEnabled && !buttonInfo.remapping.longPressEnabled) {
						//LOG(INFO) << "buttonInfo.state = 0: sendDigitalBinding - EventType: " << (int)eventType;
						sendDigitalBinding(buttonInfo.remapping.binding, unWhichDevice, eventType, eButtonId, eventTimeOffset, &buttonInfo.bindings[0]);
					} else if (eventType == ButtonEventType::ButtonPressed) {
						if (buttonInfo.remapping.longPressEnabled) {
							buttonInfo.timeout = std::chrono::system_clock::now() + std::chrono::milliseconds(buttonInfo.remapping.longPressThreshold);
						}
						buttonInfo.state = 1;
						//LOG(INFO) << "buttonInfo.state = 0: => 1";
					}
				} break;
				case 1: {
					if (eventType == ButtonEventType::ButtonUnpressed) {
						if (buttonInfo.remapping.doublePressEnabled) {
							buttonInfo.timeout = std::chrono::system_clock::now() + std::chrono::milliseconds(buttonInfo.remapping.doublePressThreshold);
							buttonInfo.state = 3;
							//LOG(INFO) << "buttonInfo.state = 1: => 3";
						} else {
							sendDigitalBinding(buttonInfo.remapping.binding, m_openvrId, ButtonEventType::ButtonPressed, eButtonId, 0.0, &buttonInfo.bindings[0]);
							buttonInfo.timeout = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
							buttonInfo.state = 4;
							//LOG(INFO) << "buttonInfo.state = 1: => 4";
						}
					}
				} break;
				case 2: {
					if (eventType == ButtonEventType::ButtonUnpressed) {
						sendDigitalBinding(buttonInfo.remapping.longPressBinding, unWhichDevice, eventType, eButtonId, eventTimeOffset, &buttonInfo.bindings[1]);
						buttonInfo.state = 0;
						//LOG(INFO) << "buttonInfo.state = 2: sendDigitalBinding, => 0";
					}
				} break;
				case 3: {
					if (eventType == ButtonEventType::ButtonPressed) {
						sendDigitalBinding(buttonInfo.remapping.doublePressBinding, unWhichDevice, eventType, eButtonId, eventTimeOffset, &buttonInfo.bindings[2]);
						if (buttonInfo.remapping.doublePressImmediateRelease) {
							buttonInfo.timeout = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
						}
						buttonInfo.state = 5;
						//LOG(INFO) << "buttonInfo.state = 3: sendDigitalBinding, => 5";
					}
				} break;
				case 5: {
					if (eventType == ButtonEventType::ButtonUnpressed) {
						sendDigitalBinding(buttonInfo.remapping.doublePressBinding, unWhichDevice, eventType, eButtonId, eventTimeOffset, &buttonInfo.bindings[2]);
						buttonInfo.state = 0;
						//LOG(INFO) << "buttonInfo.state = 5: sendDigitalBinding, => 0";
					}
				} break;
				case 6: {
					if (eventType == ButtonEventType::ButtonUnpressed) {
						buttonInfo.state = 0;
						//LOG(INFO) << "buttonInfo.state = 6: => 0";
					}
				} break;
				default: {
				} break;
				}
			}
		} else {
			if (m_deviceMode == 1 || (m_deviceMode == 3 && !m_redirectSuspended) /*|| m_deviceMode == 5*/) {
				//nop
			} else {
				sendButtonEvent(unWhichDevice, eventType, eButtonId, eventTimeOffset);
			}
		}
	} else {
		if (m_deviceMode == 1 || (m_deviceMode == 3 && !m_redirectSuspended) /*|| m_deviceMode == 5*/) {
			//nop
		} else {
			sendButtonEvent(unWhichDevice, eventType, eButtonId, eventTimeOffset);
		}
	}
}


void OpenvrDeviceManipulationInfo::suspendRedirectMode() {
	if (m_deviceMode == 2 || m_deviceMode == 3) {
		m_redirectSuspended = !m_redirectSuspended;
		_disconnectedMsgSend = false;
		m_redirectRef->m_redirectSuspended = m_redirectSuspended;
		m_redirectRef->_disconnectedMsgSend = false;
	}
}


void OpenvrDeviceManipulationInfo::RunFrame() {
	for (auto& r : m_digitalInputRemapping) {
		switch (r.second.state) {
		case 1: {
			if (r.second.remapping.longPressEnabled) {
				auto now = std::chrono::system_clock::now();
				if (r.second.timeout <= now) {
					sendDigitalBinding(r.second.remapping.longPressBinding, m_openvrId, ButtonEventType::ButtonPressed, (vr::EVRButtonId)r.first, 0.0, &r.second.bindings[1]);
					if (r.second.remapping.longPressImmediateRelease) {
						r.second.timeout = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
					}
					r.second.state = 2;
					//LOG(INFO) << "buttonInfo.state = 1: sendDigitalBinding, => 2";
				}
			}
		} break;
		case 2: {
			if (r.second.remapping.longPressImmediateRelease) {
				auto now = std::chrono::system_clock::now();
				if (r.second.timeout <= now) {
					sendDigitalBinding(r.second.remapping.longPressBinding, m_openvrId, ButtonEventType::ButtonUnpressed, (vr::EVRButtonId)r.first, 0.0, &r.second.bindings[1]);
					r.second.state = 6;
					//LOG(INFO) << "buttonInfo.state = 2: sendDigitalBinding, => 6";
				}
			}
		} break;
		case 3: {
			auto now = std::chrono::system_clock::now();
			if (r.second.timeout <= now) {
				sendDigitalBinding(r.second.remapping.binding, m_openvrId, ButtonEventType::ButtonPressed, (vr::EVRButtonId)r.first, 0.0, &r.second.bindings[0]);
				r.second.timeout = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
				r.second.state = 4;
				//LOG(INFO) << "buttonInfo.state = 3: sendDigitalBinding, => 4";
			}
		} break;
		case 4: {
			auto now = std::chrono::system_clock::now();
			if (r.second.timeout <= now) {
				sendDigitalBinding(r.second.remapping.binding, m_openvrId, ButtonEventType::ButtonUnpressed, (vr::EVRButtonId)r.first, 0.0, &r.second.bindings[0]);
				r.second.state = 0;
				//LOG(INFO) << "buttonInfo.state = 4: sendDigitalBinding, => 0";
			}
		} break;
		case 5: {
			if (r.second.remapping.doublePressImmediateRelease) {
				auto now = std::chrono::system_clock::now();
				if (r.second.timeout <= now) {
					sendDigitalBinding(r.second.remapping.doublePressBinding, m_openvrId, ButtonEventType::ButtonUnpressed, (vr::EVRButtonId)r.first, 0.0, &r.second.bindings[2]);
					r.second.state = 6;
					//LOG(INFO) << "buttonInfo.state = 5: sendDigitalBinding, => 6";
				}
			}
		} break;
		default:
			break;
		}
		RunFrameDigitalBinding(r.second.remapping.binding, (vr::EVRButtonId)r.first, r.second.bindings[0]);
		RunFrameDigitalBinding(r.second.remapping.longPressBinding, (vr::EVRButtonId)r.first, r.second.bindings[1]);
		RunFrameDigitalBinding(r.second.remapping.doublePressBinding, (vr::EVRButtonId)r.first, r.second.bindings[2]);
	}
}


void OpenvrDeviceManipulationInfo::RunFrameDigitalBinding(vrinputemulator::DigitalBinding& binding, vr::EVRButtonId eButtonId, OpenvrDeviceManipulationInfo::DigitalInputRemappingInfo::BindingInfo& bindingInfo) {
	if (bindingInfo.autoTriggerEnabled) {
		auto now = std::chrono::system_clock::now();
		if (bindingInfo.autoTriggerState && bindingInfo.autoTriggerUnpressTimeout < now) {
			bindingInfo.autoTriggerState = false;
			sendDigitalBinding(binding, m_openvrId, ButtonEventType::ButtonUnpressed, eButtonId, 0.0);
		} else if (!bindingInfo.autoTriggerState && bindingInfo.autoTriggerTimeout < now) {
			bindingInfo.autoTriggerState = true;
			auto now = std::chrono::system_clock::now();
			bindingInfo.autoTriggerUnpressTimeout = now + std::chrono::milliseconds(10);
			bindingInfo.autoTriggerTimeout = now + std::chrono::milliseconds(bindingInfo.autoTriggerTimeoutTime);
			sendDigitalBinding(binding, m_openvrId, ButtonEventType::ButtonPressed, eButtonId, 0.0);
		}
	}
	switch (bindingInfo.state) {
		case 1: {
			if (binding.toggleEnabled) {
				auto now = std::chrono::system_clock::now();
				if (bindingInfo.timeout <= now) {
					bindingInfo.state = 2;
				}
			}
		} break;
		default: {
		} break;
	}
}


void OpenvrDeviceManipulationInfo::handleAxisEvent(vr::IVRServerDriverHost* driver, _DetourTrackedDeviceAxisUpdated_t origFunc, uint32_t& unWhichDevice, uint32_t unWhichAxis, const vr::VRControllerAxis_t& axisState) {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	AnalogInputRemappingInfo* axisInfo = nullptr;
	if (unWhichAxis < 5) {
		axisInfo = m_analogInputRemapping + unWhichAxis;
	}
	if (axisInfo && axisInfo->remapping.valid) {
		sendAnalogBinding(axisInfo->remapping.binding, unWhichDevice, unWhichAxis, axisState, &axisInfo->binding);
	} else {
		if (m_deviceMode == 1 || (m_deviceMode == 3 && !m_redirectSuspended) /*|| m_deviceMode == 5*/) {
			//nop
		} else {
			sendAxisEvent(unWhichDevice, unWhichAxis, axisState);
		}
	}
}


bool OpenvrDeviceManipulationInfo::triggerHapticPulse(uint32_t unAxisId, uint16_t usPulseDurationMicroseconds, bool directMode) {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	if (m_controllerComponent) {
		if (directMode) {
			return m_triggerHapticPulseFunc(m_controllerComponent, unAxisId, usPulseDurationMicroseconds);
		} else if ((m_deviceMode == 3 && !m_redirectSuspended) || m_deviceMode == 4) {
			m_redirectRef->triggerHapticPulse(unAxisId, usPulseDurationMicroseconds, true);
		} else  if (m_deviceMode == 0 || ((m_deviceMode == 3 || m_deviceMode == 2) && m_redirectSuspended)) {
			return m_triggerHapticPulseFunc(m_controllerComponent, unAxisId, usPulseDurationMicroseconds);
		}
	}
	return true;
}



void OpenvrDeviceManipulationInfo::sendDigitalBinding(vrinputemulator::DigitalBinding& binding, uint32_t unWhichDevice, ButtonEventType eventType, 
		vr::EVRButtonId eButtonId, double eventTimeOffset, DigitalInputRemappingInfo::BindingInfo* bindingInfo) {
	if (binding.type == DigitalBindingType::NoRemapping) {
		sendButtonEvent(unWhichDevice, eventType, eButtonId, eventTimeOffset, false, bindingInfo);
	} else if (binding.type == DigitalBindingType::Disabled) {
		// nop
	} else {
		bool sendEvent = false;
		if (!bindingInfo) {
			sendEvent = true;
		} else if (eventType == ButtonEventType::ButtonPressed || eventType == ButtonEventType::ButtonUnpressed) {
			switch (bindingInfo->state) {
				case 0:  {
					int newState = 1;
					if (eventType == ButtonEventType::ButtonPressed) {
						sendEvent = true;
						if (binding.toggleEnabled) {
							if (binding.toggleDelay == 0) {
								newState = 2;
							} else {
								bindingInfo->timeout = std::chrono::system_clock::now() + std::chrono::milliseconds(binding.toggleDelay);
							}
						}
						bindingInfo->autoTriggerEnabled = binding.autoTriggerEnabled;
						if (bindingInfo->autoTriggerEnabled) {
							bindingInfo->autoTriggerState = true;
							bindingInfo->autoTriggerTimeoutTime = (uint32_t)(1000.0 / ((float)binding.autoTriggerFrequency / 100.0));
							auto now = std::chrono::system_clock::now();
							bindingInfo->autoTriggerUnpressTimeout = now + std::chrono::milliseconds(10);
							bindingInfo->autoTriggerTimeout = now + std::chrono::milliseconds(bindingInfo->autoTriggerTimeoutTime);
						}
						bindingInfo->state = newState;
					}
				} break;
				case 1: {
					if (eventType == ButtonEventType::ButtonUnpressed) {
						sendEvent = true;
						if (bindingInfo->autoTriggerEnabled) {
							bindingInfo->autoTriggerEnabled = false;
							if (!bindingInfo->autoTriggerState) {
								bindingInfo->pressedState = false;
							}
						}
						bindingInfo->state = 0;
					}
				} break;
				case 2: {
					if (eventType == ButtonEventType::ButtonPressed) {
						bindingInfo->state = 3;
					}
				} break;
				case 3: {
					if (eventType == ButtonEventType::ButtonUnpressed) {
						sendEvent = true;
						if (bindingInfo->autoTriggerEnabled) {
							bindingInfo->autoTriggerEnabled = false;
							bindingInfo->autoTriggerState = false;
						}
						bindingInfo->state = 0;
					}
				} break;
				default: {
				} break;
			}
		} else if (eventType == ButtonEventType::ButtonTouched || eventType == ButtonEventType::ButtonUntouched) {
			sendEvent = true;
		}
		if (sendEvent) {
			switch (binding.type) {
				case DigitalBindingType::OpenVR: {
					if (m_deviceMode == 1 || (m_deviceMode == 3 && !m_redirectSuspended) /*|| m_deviceMode == 5*/) {
						//nop
					} else {
						vr::EVRButtonId button = (vr::EVRButtonId)binding.data.openvr.buttonId;
						uint32_t deviceId = binding.data.openvr.controllerId;
						if (deviceId >= 999) {
							deviceId = m_openvrId;
						}
						sendButtonEvent(deviceId, eventType, button, eventTimeOffset, false, bindingInfo);
					}
				} break;
				case DigitalBindingType::Keyboard: {
					if (m_deviceMode == 1 || (m_deviceMode == 3 && !m_redirectSuspended) /*|| m_deviceMode == 5*/) {
						//nop
					} else {
						sendKeyboardEvent(eventType, binding.data.keyboard.shiftPressed, binding.data.keyboard.ctrlPressed, 
							binding.data.keyboard.altPressed, (WORD)binding.data.keyboard.keyCode, bindingInfo);
					}
				} break;
				case DigitalBindingType::SuspendRedirectMode: {
					if (eventType == ButtonEventType::ButtonPressed) {
						//_vibrationCue(); // Better not let it interfere with an haptic event triggered by an application
						_audioCue();
						suspendRedirectMode();
						bindingInfo->pressedState = true;
					} else if (eventType == ButtonEventType::ButtonUnpressed) {
						bindingInfo->pressedState = false;
					}
				} break;
				case DigitalBindingType::ToggleTouchpadEmulationFix: {
					if (eventType == ButtonEventType::ButtonPressed) {
						//_vibrationCue(); // Better not let it interfere with an haptic event triggered by an application
						_audioCue();
						OpenvrDeviceManipulationInfo::setTouchpadEmulationFixFlag(!OpenvrDeviceManipulationInfo::getTouchpadEmulationFixFlag());
						bindingInfo->pressedState = true;
					} else if (eventType == ButtonEventType::ButtonUnpressed) {
						bindingInfo->pressedState = false;
					}
				} break;
				default: {
				} break;
			}
		}
	}
}


void OpenvrDeviceManipulationInfo::_audioCue() {
	auto audiofile = CServerDriver::getInstallDirectory().append("\\resources\\sounds\\audiocue.wav");
	PlaySoundA(audiofile.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}


void OpenvrDeviceManipulationInfo::_vibrationCue() {
	if (_vibrationCueTheadHandle != NULL) {
		DWORD threadExitCode = 0;
		if (GetExitCodeThread(_vibrationCueTheadHandle, &threadExitCode) != 0 && threadExitCode == STILL_ACTIVE) {
			return;
		}
		CloseHandle(_vibrationCueTheadHandle);
		_vibrationCueTheadHandle = NULL;
	}
	static struct _vibrationCueThreadArgs {
		OpenvrDeviceManipulationInfo* deviceInfo;
	} threadArgs;
	threadArgs.deviceInfo = this;
	_vibrationCueTheadHandle = CreateThread(
		NULL,                   // default security attributes
		0,                      // use default stack size  
		[](LPVOID lpParam)->DWORD {
			_vibrationCueThreadArgs* args = (_vibrationCueThreadArgs*)lpParam;
			for (unsigned i = 0; i < 10; ++i) {
				args->deviceInfo->m_triggerHapticPulseFunc(args->deviceInfo->controllerComponent(), 0, 1000);
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return 0;
		},
		&threadArgs,			// argument to thread function 
		0,                      // use default creation flags 
		NULL
	);
}


void OpenvrDeviceManipulationInfo::sendAnalogBinding(vrinputemulator::AnalogBinding& binding, uint32_t unWhichDevice, uint32_t axisId, const vr::VRControllerAxis_t& axisState, OpenvrDeviceManipulationInfo::AnalogInputRemappingInfo::BindingInfo* bindingInfo) {
	if (binding.type == AnalogBindingType::NoRemapping) {
		sendAxisEvent(unWhichDevice, axisId, axisState, false, bindingInfo);
	} else if (binding.type == AnalogBindingType::Disabled) {
		// nop
	} else {
		switch (binding.type) {
			case AnalogBindingType::OpenVR: {
				if (m_deviceMode == 1 || (m_deviceMode == 3 && !m_redirectSuspended) /*|| m_deviceMode == 5*/) {
					//nop
				} else {
					vr::EVRButtonId axisId = (vr::EVRButtonId)binding.data.openvr.axisId;
					uint32_t deviceId = binding.data.openvr.controllerId;
					auto newAxisState = axisState;
					if (deviceId >= 999) {
						deviceId = m_openvrId;
					}
					if (binding.invertXAxis) {
						newAxisState.x *= -1;
					}
					if (binding.invertYAxis) {
						newAxisState.y *= -1;
					}
					if (binding.swapAxes) {
						auto tmp = newAxisState.x;
						newAxisState.x = newAxisState.y;
						newAxisState.y = tmp;
					}
					if ( binding.lowerDeadzone < binding.upperDeadzone && (binding.lowerDeadzone > 0.0f || binding.upperDeadzone < 1.0f)) {
						auto zone = binding.upperDeadzone - binding.lowerDeadzone;
						if (axisState.x >= 0.0f) {
							newAxisState.x = (axisState.x - binding.lowerDeadzone) * (1.0f / zone) + binding.lowerDeadzone;
							if (newAxisState.x < 0.0f) {
								newAxisState.x = 0.0f;
							} else if (newAxisState.x > 1.0f) {
								newAxisState.x = 1.0f;
							}
						} else {
							newAxisState.x = (axisState.x + binding.lowerDeadzone) * (1.0f / zone) - binding.lowerDeadzone;
							if (newAxisState.x > 0.0f) {
								newAxisState.x = 0.0f;
							} else if (newAxisState.x < -1.0f) {
								newAxisState.x = -1.0f;
							}
						}
						if (axisState.y >= 0.0f) {
							newAxisState.y = (axisState.y - binding.lowerDeadzone) * (1.0f / zone) + binding.lowerDeadzone;
							if (newAxisState.y < 0.0f) {
								newAxisState.y = 0.0f;
							} else if (newAxisState.y > 1.0f) {
								newAxisState.y = 1.0f;
							}
						} else {
							newAxisState.y = (axisState.y + binding.lowerDeadzone) * (1.0f / zone) - binding.lowerDeadzone;
							if (newAxisState.y > 0.0f) {
								newAxisState.y = 0.0f;
							} else if (newAxisState.y < -1.0f) {
								newAxisState.y = -1.0f;
							}
						}
					}
					sendAxisEvent(deviceId, axisId, newAxisState, false, bindingInfo);
				}
			} break;
			default: {
			} break;
		}
	}
}


void OpenvrDeviceManipulationInfo::_buttonPressDeadzoneFix(vr::EVRButtonId eButtonId) {
	auto& axisInfo = m_analogInputRemapping[eButtonId - vr::k_EButton_Axis0];
	if (axisInfo.remapping.valid && axisInfo.remapping.binding.buttonPressDeadzoneFix) {
		if (axisInfo.binding.lastSendAxisState.x == 0.0f && axisInfo.binding.lastSendAxisState.y == 0.0f) {
			CServerDriver::openvrTrackedDeviceAxisUpdated(m_driverHost, m_openvrId, eButtonId - vr::k_EButton_Axis0, {0.01f, 0.01f});
		}
	}
}

void OpenvrDeviceManipulationInfo::sendButtonEvent(uint32_t& unWhichDevice, ButtonEventType eventType, vr::EVRButtonId eButtonId, double eventTimeOffset, bool directMode, DigitalInputRemappingInfo::BindingInfo* binding) {
	if (!directMode) {
		if (unWhichDevice == m_openvrId && ((m_deviceMode == 2 && !m_redirectSuspended) || m_deviceMode == 4)) {
			unWhichDevice = m_redirectRef->openvrId();
		}
		if (unWhichDevice != m_openvrId) {
			auto deviceInfo = CServerDriver::getDeviceInfo(unWhichDevice);
			if (deviceInfo) {
				return deviceInfo->sendButtonEvent(unWhichDevice, eventType, eButtonId, eventTimeOffset, true, binding);
			}
		}
	}
	switch (eventType) {
		case ButtonEventType::ButtonPressed: {
			if (binding) {
				if (!binding->touchedState) {
					if (eButtonId >= vr::k_EButton_Axis0 && eButtonId <= vr::k_EButton_Axis4) {
						auto& axisInfo = m_analogInputRemapping[eButtonId - vr::k_EButton_Axis0];
						axisInfo.binding.touchedState = true;
					}
					CServerDriver::openvrTrackedDeviceButtonTouched(m_driverHost, unWhichDevice, eButtonId, eventTimeOffset);
					binding->touchedState = true;
					binding->touchedAutoset = true;
				}
				if (!binding->pressedState) {
					if (eButtonId >= vr::k_EButton_Axis0 && eButtonId <= vr::k_EButton_Axis4) {
						_buttonPressDeadzoneFix(eButtonId);
					}
					CServerDriver::openvrTrackedDeviceButtonPressed(m_driverHost, unWhichDevice, eButtonId, eventTimeOffset);
					binding->pressedState = true;
				}
			} else {
				if (eButtonId >= vr::k_EButton_Axis0 && eButtonId <= vr::k_EButton_Axis4) {
					_buttonPressDeadzoneFix(eButtonId);
				}
				CServerDriver::openvrTrackedDeviceButtonPressed(m_driverHost, unWhichDevice, eButtonId, eventTimeOffset);
			}
		} break;
		case ButtonEventType::ButtonUnpressed: {
			if (binding) {
				if (binding->pressedState) {
					CServerDriver::openvrTrackedDeviceButtonUnpressed(m_driverHost, unWhichDevice, eButtonId, eventTimeOffset);
					binding->pressedState = false;
				}
				if (binding->touchedState && binding->touchedAutoset) {
					CServerDriver::openvrTrackedDeviceButtonUntouched(m_driverHost, unWhichDevice, eButtonId, eventTimeOffset);
					binding->touchedState = false;
					binding->touchedAutoset = false;
				}
			} else {
				CServerDriver::openvrTrackedDeviceButtonUnpressed(m_driverHost, unWhichDevice, eButtonId, eventTimeOffset);
			}
		} break;
		case ButtonEventType::ButtonTouched: {
			if (binding) {
				if (!binding->touchedState) {
					CServerDriver::openvrTrackedDeviceButtonTouched(m_driverHost, unWhichDevice, eButtonId, eventTimeOffset);
					binding->touchedState = true;
					binding->touchedAutoset = false;
				}
			} else {
				CServerDriver::openvrTrackedDeviceButtonTouched(m_driverHost, unWhichDevice, eButtonId, eventTimeOffset);
			}
		} break;
		case ButtonEventType::ButtonUntouched: {
			if (binding) {
				if (binding->touchedState) {
					if (binding->pressedState) {
						binding->touchedAutoset = true;
					} else {
						CServerDriver::openvrTrackedDeviceButtonUntouched(m_driverHost, unWhichDevice, eButtonId, eventTimeOffset);
						binding->touchedState = false;
						binding->touchedAutoset = false;
					}
				}
			} else {
				CServerDriver::openvrTrackedDeviceButtonUntouched(m_driverHost, unWhichDevice, eButtonId, eventTimeOffset);
			}
		} break;
		default: {
		} break;
	}
}

void OpenvrDeviceManipulationInfo::sendKeyboardEvent(ButtonEventType eventType, bool shiftPressed, bool ctrlPressed, bool altPressed, WORD keyCode, DigitalInputRemappingInfo::BindingInfo* binding) {
	if ( (eventType == ButtonEventType::ButtonPressed && (!binding || !binding->pressedState)) 
			|| (eventType == ButtonEventType::ButtonUnpressed && (!binding || binding->pressedState)) ) {
		INPUT ips[4];
		memset(ips, 0, sizeof(ips));
		/* DirectInput games ignore virtual key codes, so we have to send the scan code */
		WORD flags = KEYEVENTF_SCANCODE;
		if (eventType == ButtonEventType::ButtonUnpressed) {
			flags |= KEYEVENTF_KEYUP;
		}
		auto scanCode = MapVirtualKey(keyCode, MAPVK_VK_TO_VSC);
		unsigned ipCount = 0;
		if (eventType == ButtonEventType::ButtonUnpressed && scanCode) {
			ips[ipCount].type = INPUT_KEYBOARD;
			ips[ipCount].ki.wScan = scanCode;
			ips[ipCount].ki.dwFlags = flags;
			ipCount++;
		}
		if (shiftPressed) {
			ips[ipCount].type = INPUT_KEYBOARD;
			ips[ipCount].ki.wScan = MapVirtualKey(VK_SHIFT, MAPVK_VK_TO_VSC);
			ips[ipCount].ki.dwFlags = flags;
			ipCount++;
		}
		if (ctrlPressed) {
			ips[ipCount].type = INPUT_KEYBOARD;
			ips[ipCount].ki.wScan = MapVirtualKey(VK_CONTROL, MAPVK_VK_TO_VSC);
			ips[ipCount].ki.dwFlags = flags;
			ipCount++;
		}
		if (altPressed) {
			ips[ipCount].type = INPUT_KEYBOARD;
			ips[ipCount].ki.wScan = MapVirtualKey(VK_MENU, MAPVK_VK_TO_VSC);
			ips[ipCount].ki.dwFlags = flags;
			ipCount++;
		}
		if (eventType == ButtonEventType::ButtonPressed && scanCode) {
			ips[ipCount].type = INPUT_KEYBOARD;
			ips[ipCount].ki.wScan = scanCode;
			ips[ipCount].ki.dwFlags = flags;
			ipCount++;
		}
		SendInput(4, ips, sizeof(INPUT));
		if (binding) {
			if (eventType == ButtonEventType::ButtonPressed) {
				binding->pressedState = true;
			} else if (eventType == ButtonEventType::ButtonUnpressed) {
				binding->pressedState = false;
			}
		}
	}
}


void OpenvrDeviceManipulationInfo::sendAxisEvent(uint32_t& unWhichDevice, uint32_t unWhichAxis, const vr::VRControllerAxis_t& axisState, bool directMode, AnalogInputRemappingInfo::BindingInfo* binding) {
	if (!directMode) {
		if (unWhichDevice == m_openvrId && ((m_deviceMode == 2 && !m_redirectSuspended) || m_deviceMode == 4)) {
			unWhichDevice = m_redirectRef->openvrId();
		}
		if (unWhichDevice != m_openvrId) {
			auto deviceInfo = CServerDriver::getDeviceInfo(unWhichDevice);
			if (deviceInfo) {
				return deviceInfo->sendAxisEvent(unWhichDevice, unWhichAxis, axisState, true, binding);
			}
		}
	}
	if (unWhichAxis < 5) {
		if (m_analogInputRemapping[unWhichAxis].remapping.valid && touchpadEmulationEnabledFlag) {
			auto newState = axisState;
			auto& lastSeenState = m_analogInputRemapping[unWhichAxis].binding.lastSeenAxisState;
			auto& lastSendState = m_analogInputRemapping[unWhichAxis].binding.lastSendAxisState;
			bool suppress = false;

			if (m_analogInputRemapping[unWhichAxis].remapping.binding.touchpadEmulationMode == 1) {
				if (axisState.x != 0.0f && vrmath::signum(axisState.x) == vrmath::signum(lastSendState.x) && abs(axisState.x) < abs(lastSendState.x)) {
					newState.x = lastSendState.x;
				}
				if (axisState.y != 0.0f && vrmath::signum(axisState.y) == vrmath::signum(lastSendState.y) && abs(axisState.y) < abs(lastSendState.y)) {
					newState.y = lastSendState.y;
				}

			} else if (m_analogInputRemapping[unWhichAxis].remapping.binding.touchpadEmulationMode == 2) {
				// Joystick was already in neutral position but we haven't send it yet, and now it moved away from neutral position
				// => send neutral position before we do anything else since some menus use this information to reset input handling.
				if (lastSeenState.x == 0.0f && lastSeenState.y == 0.0f && (lastSendState.x != 0.0f || lastSendState.y != 0.0f) && (axisState.x != 0.0f || axisState.y != 0.0f)) {
					CServerDriver::openvrTrackedDeviceAxisUpdated(m_driverHost, unWhichDevice, unWhichAxis, {0.0f, 0.0f});
					lastSendState = {0.0f, 0.0f};
				}
				if (axisState.x == 0.0f || (vrmath::signum(axisState.x) == vrmath::signum(lastSendState.x) && abs(axisState.x) < abs(lastSendState.x))) {
					newState.x = lastSendState.x;
				}
				if (axisState.y == 0.0f || (vrmath::signum(axisState.y) == vrmath::signum(lastSendState.y) && abs(axisState.y) < abs(lastSendState.y))) {
					newState.y = lastSendState.y;
				}
			}

			lastSeenState = axisState;
			if (!suppress) {
				CServerDriver::openvrTrackedDeviceAxisUpdated(m_driverHost, unWhichDevice, unWhichAxis, newState);
				lastSendState = newState;
			}
		} else {
			CServerDriver::openvrTrackedDeviceAxisUpdated(m_driverHost, unWhichDevice, unWhichAxis, axisState);
			m_analogInputRemapping[unWhichAxis].binding.lastSeenAxisState = axisState;
			m_analogInputRemapping[unWhichAxis].binding.lastSendAxisState = axisState;
		}

	} else {
		CServerDriver::openvrTrackedDeviceAxisUpdated(m_driverHost, unWhichDevice, unWhichAxis, axisState);
	}
}


/*bool OpenvrDeviceManipulationInfo::getButtonMapping(vr::EVRButtonId button, vr::EVRButtonId& mappedButton) {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	if (m_enableButtonMapping) {
		auto i = m_buttonMapping.find(button);
		if (i != m_buttonMapping.end()) {
			mappedButton = i->second;
			return true;
		}
	}
	return false;
}

void OpenvrDeviceManipulationInfo::addButtonMapping(vr::EVRButtonId button, vr::EVRButtonId mappedButton) {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	m_buttonMapping[button] = mappedButton;
}

void OpenvrDeviceManipulationInfo::eraseButtonMapping(vr::EVRButtonId button) {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	m_buttonMapping.erase(button);
}

void OpenvrDeviceManipulationInfo::eraseAllButtonMappings() {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	m_buttonMapping.clear();
}*/

int OpenvrDeviceManipulationInfo::setDefaultMode() {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	auto res = _disableOldMode(0);
	if (res == 0) {
		m_deviceMode = 0;
	}
	return 0; 
}

int OpenvrDeviceManipulationInfo::setRedirectMode(bool target, OpenvrDeviceManipulationInfo* ref) {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	auto res = _disableOldMode(target ? 3 : 2);
	if (res == 0) {
		m_redirectSuspended = false;
		m_redirectRef = ref;
		if (target) {
			m_deviceMode = 3;
		} else {
			m_deviceMode = 2;
		}
	}
	return 0; 
}

int OpenvrDeviceManipulationInfo::setSwapMode(OpenvrDeviceManipulationInfo* ref) {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	auto res = _disableOldMode(4);
	if (res == 0) {
		m_redirectRef = ref;
		m_deviceMode = 4;
	}
	return 0;
}

int OpenvrDeviceManipulationInfo::setMotionCompensationMode() {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	auto res = _disableOldMode(5);
	auto serverDriver = CServerDriver::getInstance();
	if (res == 0 && serverDriver) {
		serverDriver->enableMotionCompensation(true);
		serverDriver->setMotionCompensationRefDevice(this);
		serverDriver->_setMotionCompensationStatus(MotionCompensationStatus::WaitingForZeroRef);
		m_deviceMode = 5;
	}
	return 0;
}

int OpenvrDeviceManipulationInfo::setFakeDisconnectedMode() {
	std::lock_guard<std::recursive_mutex> lock(_mutex);
	auto res = _disableOldMode(1);
	if (res == 0) {
		_disconnectedMsgSend = false;
		m_deviceMode = 1;
	}
	return 0;
}

int OpenvrDeviceManipulationInfo::_disableOldMode(int newMode) {
	if (m_deviceMode != newMode) {
		if (m_deviceMode == 5) {
			auto serverDriver = CServerDriver::getInstance();
			if (serverDriver) {
				serverDriver->enableMotionCompensation(false);
				serverDriver->setMotionCompensationRefDevice(nullptr);
			}
		} else if (m_deviceMode == 3 || m_deviceMode == 2 || m_deviceMode == 4) {
			m_redirectRef->m_deviceMode = 0;
		}
		if (newMode == 5) {
			auto serverDriver = CServerDriver::getInstance();
			if (serverDriver) {
				serverDriver->_disableMotionCompensationOnAllDevices();
				serverDriver->enableMotionCompensation(false);
				serverDriver->setMotionCompensationRefDevice(nullptr);
			}
		} 
	}
	return 0;
}


void OpenvrDeviceManipulationInfo::setControllerComponent(vr::IVRControllerComponent* component, _DetourTriggerHapticPulse_t triggerHapticPulse) {
	m_controllerComponent = component;
	m_triggerHapticPulseFunc = triggerHapticPulse;
}

} // end namespace driver
} // end namespace vrinputemulator
