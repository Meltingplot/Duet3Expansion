/*
 * FilamentSensor.cpp
 *
 *  Created on: 20 Jul 2017
 *      Author: David
 */

#include "FilamentMonitor.h"

#if SUPPORT_DRIVERS

#include "SimpleFilamentMonitor.h"
#include "RotatingMagnetFilamentMonitor.h"
#include "LaserFilamentMonitor.h"
#include "PulsedFilamentMonitor.h"
#include <Platform/Platform.h>
#include <Movement/Move.h>
#include <CAN/CanInterface.h>
#include <CanMessageFormats.h>
#include <CanMessageBuffer.h>
#include <CanMessageGenericParser.h>
#include <CanMessageGenericTables.h>

#if SUPPORT_AS5601
# include <CommandProcessing/MFMHandler.h>
#endif

// Static data
ReadWriteLock FilamentMonitor::filamentMonitorsLock;
FilamentMonitor *FilamentMonitor::filamentSensors[NumDrivers] = { 0 };
uint32_t FilamentMonitor::whenStatusLastSent = 0;
size_t FilamentMonitor::firstDriveToSend = 0;
uint32_t FilamentMonitor::staticMaxPollInterval = NoMonitorsConfigured;
uint32_t FilamentMonitor::staticMinPollInterval = 0;
uint32_t FilamentMonitor::whenAnySpinRan = 0;
volatile bool FilamentMonitor::anyInterruptSeen = false;
#if FILAMENT_MONITOR_TIMING_DIAGNOSTICS
uint32_t FilamentMonitor::minInterruptTime = 0xFFFFFFFF, FilamentMonitor::maxInterruptTime = 0;
uint32_t FilamentMonitor::minPollTime = 0xFFFFFFFF, FilamentMonitor::maxPollTime = 0;
#endif

// Constructor
FilamentMonitor::FilamentMonitor(uint8_t p_driver, unsigned int t) noexcept
	: type(t), driver(p_driver), enableMode(0), lastStatus(FilamentSensorStatus::noDataReceived), lastReportedLiveBits(0)
{
}

// Recompute the intervals that the fast path in Spin uses. Caller must hold the write lock.
// If any configured monitor has not opted into throttling we must never skip, so the result is 0.
/*static*/ void FilamentMonitor::UpdateStaticPollInterval() noexcept
{
	uint32_t maxInterval = NoMonitorsConfigured;
	uint32_t minInterval = NoMonitorsConfigured;
	for (const FilamentMonitor *const fs : filamentSensors)
	{
		if (fs != nullptr)
		{
			if (fs->maxPollInterval == 0)
			{
				maxInterval = 0;										// this monitor wants polling every time, so nobody gets skipped
				break;
			}
			if (maxInterval == NoMonitorsConfigured || fs->maxPollInterval < maxInterval)
			{
				maxInterval = fs->maxPollInterval;
			}
			if (minInterval == NoMonitorsConfigured || fs->minPollInterval < minInterval)
			{
				minInterval = fs->minPollInterval;
			}
		}
	}

	// Store the minimum first, so that a reader in Spin that sees a usable maximum always sees a matching minimum
	staticMinPollInterval = (minInterval == NoMonitorsConfigured) ? 0 : minInterval;
	staticMaxPollInterval = maxInterval;
}

// Default destructor
FilamentMonitor::~FilamentMonitor() noexcept
{
#if SUPPORT_AS5601
	if (IsDirectMagneticEncoder())
	{
		MFMHandler::DetachEncoderVirtualInterrupt(this);
	}
#endif
}

// Call this to disable the interrupt before deleting or re-configuring a local filament monitor
void FilamentMonitor::Disable() noexcept
{
#if SUPPORT_AS5601
	if (IsDirectMagneticEncoder())
	{
		MFMHandler::DetachEncoderVirtualInterrupt(this);
	}
#endif
	port.Release();				// this also detaches the ISR
}

// Do the configuration that is common to all filament monitor types
// Try to get the pin number from the GCode command in the buffer, setting Seen if a pin number was provided and returning true if error.
// Also attaches the ISR.
// For a remote filament monitor, this does the full configuration or query of the remote object instead, and we always return seen true because we don't need to report local status.
GCodeResult FilamentMonitor::CommonConfigure(const CanMessageGenericParser& parser, const StringRef& reply, InterruptMode interruptMode, bool& seen) noexcept
{
	if (parser.GetUintParam('S', enableMode))
	{
		seen = true;
		if (enableMode > 2)
		{
			enableMode = 2;
		}
	}

	String<StringLength20> portName;
	if (parser.GetStringParam('C', portName.GetRef()))
	{
		seen = true;
		if (!port.AssignPort(portName.c_str(), reply, PinUsedBy::filamentMonitor, PinAccess::readNoDebounce))
		{
			return GCodeResult::error;
		}

		haveIsrStepsCommanded = false;

#if SUPPORT_AS5601
		if (IsDirectMagneticEncoder())
		{
			if (type != 3)
			{
				reply.copy("wrong filament monitor type for this port");
				return GCodeResult::error;
			}
			if (!MFMHandler::AttachEncoderVirtualInterrupt(AS5601VirtualInterruptEntry, this))
			{
				reply.copy("encoder not found or already in use");
				return GCodeResult::error;
			}
		}
		else
#endif
		{
			if (interruptMode != InterruptMode::none && !port.AttachInterrupt(InterruptEntry, interruptMode, CallbackParameter(this)))
			{
				reply.copy("unsuitable pin");
				return GCodeResult::error;
			}
		}
	}
	return GCodeResult::ok;
}

// Static initialisation
/*static*/ void FilamentMonitor::InitStatic() noexcept
{
	// Nothing needed here yet
}

// Create a new filament monitor, or replace an existing one
/*static*/ GCodeResult FilamentMonitor::Create(const CanMessageCreateFilamentMonitor& msg, const StringRef& reply) noexcept
{
	const uint8_t p_driver = msg.driver;
	if (p_driver >= NumDrivers)
	{
		reply.copy("Driver number out of range");
		return GCodeResult::error;
	}

	WriteLocker lock(filamentMonitorsLock);

	// Delete any existing filament monitor; Disable() must run before delete to detach the ISR while the derived vtable is still valid
	FilamentMonitor *fm = nullptr;
	std::swap(fm, filamentSensors[p_driver]);
	if (fm != nullptr)
	{
		fm->Disable();
		delete fm;
	}

	// Create the new one
	const uint8_t monitorType = msg.type;
	switch (msg.type)
	{
	case 1:		// active high switch
	case 2:		// active low switch
		fm = new SimpleFilamentMonitor(p_driver, monitorType);
		break;

	case 3:		// duet3d rotating magnet, no switch
	case 4:		// duet3d rotating magnet + switch
		fm = new RotatingMagnetFilamentMonitor(p_driver, monitorType);
		break;

	case 5:		// duet3d laser, no switch
	case 6:		// duet3d laser + switch
		fm = new LaserFilamentMonitor(p_driver, monitorType);
		break;

	case 7:		// simple pulse output sensor
		fm = new PulsedFilamentMonitor(p_driver, monitorType);
		break;

	default:	// no sensor, or unknown sensor
		reply.printf("Unknown filament monitor type %u", monitorType);
		UpdateStaticPollInterval();				// we deleted the old monitor above, so this must be refreshed on this path too
		return GCodeResult::error;
	}

	filamentSensors[p_driver] = fm;
	UpdateStaticPollInterval();
	return GCodeResult::ok;
}

// Delete a filament monitor
/*static*/ GCodeResult FilamentMonitor::Delete(const CanMessageDeleteFilamentMonitor& msg, const StringRef& reply) noexcept
{
	const uint8_t p_driver = msg.driver;
	if (p_driver >= NumDrivers)
	{
		reply.copy("Driver number out of range");
		return GCodeResult::error;
	}

	WriteLocker lock(filamentMonitorsLock);

	FilamentMonitor *fm = nullptr;
	std::swap(fm, filamentSensors[p_driver]);

	if (fm == nullptr)
	{
		reply.printf("Driver %u.%u has no filament monitor", CanInterface::GetCanAddress(), p_driver);
		return GCodeResult::warning;
	}

	fm->Disable();					// detach the ISR before destroying the derived object
	delete fm;
	UpdateStaticPollInterval();
	return GCodeResult::ok;
}

// Configure a filament monitor
/*static*/ GCodeResult FilamentMonitor::Configure(const CanMessageGeneric& msg, const StringRef& reply) noexcept
{
	CanMessageGenericParser parser(msg, ConfigureFilamentMonitorParams);
	uint8_t p_driver;
	if (!parser.GetUintParam('d', p_driver) || p_driver >= NumDrivers)
	{
		reply.copy("Bad or missing driver number");
		return GCodeResult::error;
	}

	WriteLocker lock(filamentMonitorsLock);

	FilamentMonitor *fm = filamentSensors[p_driver];
	if (fm == nullptr)
	{
		reply.printf("Driver %u.%u has no filament monitor", CanInterface::GetCanAddress(), p_driver);
		return GCodeResult::error;
	}

	return fm->Configure(parser, reply);
}

// Return an error message corresponding to a status code
/*static*/ const char *FilamentMonitor::GetErrorMessage(FilamentSensorStatus f) noexcept
{
	switch(f.RawValue())
	{
	case FilamentSensorStatus::ok:					return "no error";
	case FilamentSensorStatus::noFilament:			return "no filament";
	case FilamentSensorStatus::tooLittleMovement:	return "too little movement";
	case FilamentSensorStatus::tooMuchMovement:		return "too much movement";
	case FilamentSensorStatus::sensorError:			return "sensor not working";
	default:										return "unknown error";
	}
}

// ISR
/*static*/ void FilamentMonitor::InterruptEntry(CallbackParameter param) noexcept
{
#if FILAMENT_MONITOR_TIMING_DIAGNOSTICS
	const uint32_t startTime = StepTimer::GetTimerTicks();
#endif
	FilamentMonitor * const fm = static_cast<FilamentMonitor*>(param.vp);
	if (fm->Interrupt())
	{
		fm->isrExtruderStepsCommanded = moveInstance->GetAccumulatedExtrusion(fm->driver, fm->isrWasPrinting);
		fm->haveIsrStepsCommanded = true;
		fm->lastIsrMillis = millis();
	}
	anyInterruptSeen = true;					// let Spin past the fast path gate
#if FILAMENT_MONITOR_TIMING_DIAGNOSTICS
	const uint32_t elapsedTime = StepTimer::GetTimerTicks() - startTime;
	if (elapsedTime > maxInterruptTime)
	{
		maxInterruptTime = elapsedTime;
	}
	if (elapsedTime < minInterruptTime)
	{
		minInterruptTime = elapsedTime;
	}
#endif
}

#if SUPPORT_AS5601

// Virtual ISR from AS5601 task
/*static*/ void FilamentMonitor::AS5601VirtualInterruptEntry(CallbackParameter param) noexcept
{
	FilamentMonitor * const fm = static_cast<FilamentMonitor*>(param.vp);
	fm->isrExtruderStepsCommanded = moveInstance->GetAccumulatedExtrusion(fm->driver, fm->isrWasPrinting);
	fm->haveIsrStepsCommanded = true;
	fm->lastIsrMillis = millis();
	anyInterruptSeen = true;					// let Spin past the fast path gate
}

#endif

// Check the status of all the filament monitors.
// Currently, the status for all filament monitors (on expansion boards as well as on the main board) is checked by the main board, which generates any necessary events.
/*static*/ void FilamentMonitor::Spin() noexcept
{
	// Decide whether there is anything to do before taking the read lock or setting up the CAN message. LockForReading costs
	// two scheduler suspend/resume pairs plus a LockRecord allocation, which is the bulk of what this function costs when it
	// has nothing to do. An interrupt only brings the poll forward once staticMinPollInterval has passed, because the Duet3D
	// sensors signal on every edge, which is several times per word; staticMaxPollInterval is the backstop when nothing is
	// arriving, so that the receive state machine timeouts, the overdue check and the periodic status report still happen.
	// The intervals are plain aligned words written only under the write lock, so a stale read here costs at most one extra
	// or one skipped poll. The test and the clear of anyInterruptSeen are not atomic, so an edge recorded in between is
	// forgotten, which costs at most one interval and is absorbed by the depth of the edge capture buffer.
	{
		const uint32_t maxInterval = staticMaxPollInterval;
		if (maxInterval == NoMonitorsConfigured)
		{
			return;
		}
		if (maxInterval != 0)
		{
			const uint32_t now = millis();
			const uint32_t sinceLast = now - whenAnySpinRan;
			if (sinceLast < maxInterval && !(anyInterruptSeen && sinceLast >= staticMinPollInterval))
			{
				return;
			}
			anyInterruptSeen = false;
			whenAnySpinRan = now;
		}
	}

	CanMessageBuffer buf;
	auto msg = buf.SetupRequestMessageNoRid<CanMessageFilamentMonitorsStatusV2>(CanInterface::GetCanAddress(), CanInterface::GetCurrentMasterAddress());
	size_t slotIndex = 0;
	size_t firstDriveNotSent = NumDrivers;
	Bitmap<uint32_t> driversReported;
	bool forceSend = false, haveLiveData = false;

	{
		ReadLocker lock(filamentMonitorsLock);

		for (size_t drv = 0; drv < NumDrivers; ++drv)
		{
			if (filamentSensors[drv] != nullptr)
			{
#if FILAMENT_MONITOR_TIMING_DIAGNOSTICS
				const uint32_t startTime = StepTimer::GetTimerTicks();
#endif
				FilamentMonitor& fs = *filamentSensors[drv];
				bool isPrinting;
				bool fromIsr;
				int32_t extruderStepsCommanded;
				uint32_t locIsrMillis;
				IrqDisable();
				if (fs.haveIsrStepsCommanded)
				{
					extruderStepsCommanded = fs.isrExtruderStepsCommanded;
					isPrinting = fs.isrWasPrinting;
					locIsrMillis = fs.lastIsrMillis;
					fs.haveIsrStepsCommanded = false;
					IrqEnable();
					fromIsr = true;
				}
				else
				{
					extruderStepsCommanded = moveInstance->GetAccumulatedExtrusion(drv, isPrinting);		// get and clear the net extrusion commanded
					IrqEnable();
					fromIsr = false;
					locIsrMillis = 0;
				}

				FilamentSensorStatus fst(FilamentSensorStatus::noMonitor);
				if (fs.enableMode == 2 || Platform::IsPrinting())
				{
					const float extrusionCommanded = (float)extruderStepsCommanded/moveInstance->DriveStepsPerMm(drv);
					fst = fs.Check(isPrinting, fromIsr, locIsrMillis, extrusionCommanded);
				}
				else
				{
					fst = fs.Clear();
				}

#if FILAMENT_MONITOR_TIMING_DIAGNOSTICS
				const uint32_t elapsedTime = StepTimer::GetTimerTicks() - startTime;
				if (elapsedTime > maxPollTime)
				{
					maxPollTime = elapsedTime;
				}
				if (elapsedTime < minPollTime)
				{
					minPollTime = elapsedTime;
				}
#endif

				if (drv >= firstDriveToSend)
				{
					if (slotIndex < ARRAY_SIZE(msg->data))
					{
						auto& slot = msg->data[slotIndex];
						slot.status = fst.ToBaseType();
						fs.GetLiveData(slot);
						bool present = false;
						slot.filamentPresentValid = fs.GetLocalFilamentPresent(present);
						slot.filamentPresent = slot.filamentPresentValid && present;
						slot.motionDetected = fs.IsLocalMotionDetected();
						const uint8_t liveBits = (uint8_t)((slot.filamentPresentValid << 2) | (slot.filamentPresent << 1) | slot.motionDetected);
						if (fst != fs.lastStatus || liveBits != fs.lastReportedLiveBits)
						{
							forceSend = true;
							fs.lastStatus = fst;
							fs.lastReportedLiveBits = liveBits;
						}
						else if (slot.hasLiveData)
						{
							haveLiveData = true;
						}
						driversReported.SetBit(drv);
						++slotIndex;
					}
					else if (drv < firstDriveNotSent)
					{
						firstDriveNotSent = drv;
					}
				}
			}
		}
	}

	uint32_t now;
	if (   slotIndex != 0
		&& (   forceSend
			|| (now = millis()) - whenStatusLastSent >= StatusUpdateInterval
			|| (haveLiveData && now - whenStatusLastSent >= LiveStatusUpdateInterval)

		   )
	   )
	{
		msg->SetStandardFields(driversReported);
		buf.dataLength = msg->GetActualDataLength();
		CanInterface::Send(&buf);
		whenStatusLastSent = millis();
	}
	firstDriveToSend = (firstDriveNotSent < NumDrivers) ? firstDriveNotSent : 0;
}

// Close down the filament monitors, in particular stop them generating interrupts. Called when we are about to update firmware.
/*static*/ void FilamentMonitor::Exit() noexcept
{
	WriteLocker lock(filamentMonitorsLock);

	for (FilamentMonitor *&f : filamentSensors)
	{
		DeleteObject(f);
	}
	UpdateStaticPollInterval();
}

// Return the status of the filament sensor for a drive
/*static*/ FilamentSensorStatus FilamentMonitor::GetFilamentStatus(size_t drive)
{
	TaskCriticalSectionLocker lock;
	FilamentMonitor *fs;
	return (drive >= NumDrivers || (fs = filamentSensors[drive]) == nullptr) ? FilamentSensorStatus::noMonitor : fs->lastStatus;
}

// Send diagnostics info
/*static*/ void FilamentMonitor::GetDiagnostics(const StringRef& reply) noexcept
{
	bool first = true;
	ReadLocker lock(filamentMonitorsLock);

	for (size_t i = 0; i < NumDrivers; ++i)
	{
		FilamentMonitor * const fs = filamentSensors[i];
		if (fs != nullptr)
		{
			if (first)
			{
#if FILAMENT_MONITOR_TIMING_DIAGNOSTICS
				reply.lcatf("=== Filament sensors ===\nInterrupt %" PRIu32 " to %" PRIu32 "us, poll %" PRIu32 " to %" PRIu32 "us",
								StepTimer::TicksToIntegerMicroseconds(minInterruptTime), StepTimer::TicksToIntegerMicroseconds(maxInterruptTime),
								StepTimer::TicksToIntegerMicroseconds(minPollTime), StepTimer::TicksToIntegerMicroseconds(maxPollTime));
				minPollTime = minInterruptTime = 0xFFFFFFFF;
				maxPollTime = maxInterruptTime = 0;
#else
				reply.lcat("=== Filament sensors ===");
#endif
				first = false;
			}
			fs->Diagnostics(reply);
		}
	}
}

#endif	// SUPPORT_DRIVERS

// End
