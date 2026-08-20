/*
 * RotatingMagnetFilamentMonitor.h
 *
 *  Created on: 9 Jan 2018
 *      Author: David
 */

#ifndef SRC_FILAMENTSENSORS_ROTATINGMAGNETFILAMENTMONITOR_H_
#define SRC_FILAMENTSENSORS_ROTATINGMAGNETFILAMENTMONITOR_H_

#include "Duet3DFilamentMonitor.h"

#if SUPPORT_DRIVERS

class RotatingMagnetFilamentMonitor : public Duet3DFilamentMonitor
{
public:
	RotatingMagnetFilamentMonitor(unsigned int extruder, unsigned int monitorType) noexcept;

protected:
	GCodeResult Configure(const CanMessageGenericParser& parser, const StringRef& reply) noexcept override;
	void Diagnostics(const StringRef& reply) noexcept override;

	FilamentSensorStatus Check(bool isPrinting, bool fromIsr, uint32_t isrMillis, float filamentConsumed) noexcept override;
	FilamentSensorStatus Clear() noexcept override;
	void GetLiveData(FilamentMonitorDataNew2& data) const noexcept override;

private:
	// This sensor sends at 1kbit/s into a 64-entry edge capture buffer, and it signals an interrupt on every edge, which is
	// about 14 edges per word. Polling on each of those costs several times as many scheduler critical sections as polling
	// on a timer, for latency that nothing downstream observes. So stay interrupt driven but cap the rate at MinPollInterval,
	// and fall back to MaxPollInterval when nothing is arriving. The sensor sends a word at most every 40ms
	// (MinOutputIntervalTicks in its firmware) and a word takes 25ms, so 5ms gives eight polls per word period. The backstop
	// is safe at 20ms only because the interrupt brings the poll forward whenever data is actually arriving: the ISR will not
	// take a new extrusion snapshot until the previous one has been collected, and the next start bit is 40ms away.
	static constexpr uint32_t MinPollInterval = 5;					// milliseconds; shortest gap between interrupt-driven polls
	static constexpr uint32_t MaxPollInterval = 20;					// milliseconds; set to 0 to disable throttling altogether

	static constexpr float DefaultMmPerRev = 25.1;
	static constexpr float DefaultMinMovementAllowed = 0.6;
	static constexpr float DefaultMaxMovementAllowed = 1.6;
	static constexpr float DefaultMinimumExtrusionCheckLength = 3.0;

	// Version 1 message definitions
	static constexpr uint16_t TypeMagnetV1ErrorMask = 0x8000u;
	static constexpr uint16_t TypeMagnetV1SwitchOpenMask = 0x4000;

	// Version 2 message definitions
	static constexpr uint16_t TypeMagnetV2ParityMask = 0x8000;

	// Definitions for identifying the top level type of a message
	static constexpr uint16_t TypeMagnetV2MessageTypeMask = 0x6C00;
	static constexpr uint16_t TypeMagnetV2MessageTypePosition = 0x0800;
	static constexpr uint16_t TypeMagnetV2MessageTypeError = 0x2000;
	static constexpr uint16_t TypeMagnetV2MessageTypeInfo = 0x6000;

	// Definitions for position data messages
	static constexpr uint16_t TypeMagnetV2SwitchOpenMask = 0x1000;

	// Definitions for info message types
	static constexpr uint16_t TypeMagnetV2InfoTypeMask = 0x1F00;
	static constexpr uint16_t TypeMagnetV2InfoTypeVersion = 0x0000;
	static constexpr uint16_t TypeMagnetV3InfoTypeMagnitude = 0x0200;
	static constexpr uint16_t TypeMagnetV3InfoTypeAgc = 0x0300;

	static constexpr uint16_t TypeMagnetAngleMask = 0x03FF;			// we use a 10-bit sensor angle

#if SUPPORT_AS5601
	uint32_t LedFlashTime = 100;									// how long we flash the LED for in milliseconds
#endif

	void Init() noexcept;
	void Reset() noexcept;
	void HandleIncomingData() noexcept;
	FilamentSensorStatus CheckFilament(float amountCommanded, float amountMeasured, bool overdue) noexcept;

	bool HaveCalibrationData() const noexcept;
	float MeasuredSensitivity() const noexcept;

#if SUPPORT_AS5601
	void HandleDirectAS5601Data() noexcept;
#endif

	// Configuration parameters
	float mmPerRev;
	float minMovementAllowed, maxMovementAllowed;
	float minimumExtrusionCheckLength;
	bool checkNonPrintingMoves;

	// Other data
	uint32_t framingErrorCount;								// the number of framing errors we received
	uint32_t parityErrorCount;								// the number of words with bad parity we received
	uint32_t overdueCount;									// the number of times a position report was overdue

	uint32_t candidateStartBitTime;							// the time that we received a possible start bit
	float extrusionCommandedAtCandidateStartBit;			// the amount of extrusion commanded since the previous comparison when we received the possible start bit

	uint32_t lastSyncTime;									// the last time we took a measurement that was synced to a start bit
	float extrusionCommandedSinceLastSync;
	float movementMeasuredSinceLastSync;

#if SUPPORT_AS5601
	MillisTimer ledTimer;									// timer for flashing the LEDs
#endif

	uint16_t sensorValue;									// latest word received from sensor
	uint16_t lastKnownPosition;								// last known filament position (10 bits)
	uint32_t lastMeasurementTime;							// the last time we received a value
	uint16_t switchOpenMask;								// mask to isolate the switch open bit(s) from the sensor value
	uint8_t version;										// sensor/firmware version
	uint8_t lastErrorCode;									// the last error code received
	uint8_t magnitude;										// the last magnitude received (sensor firmware V3)
	uint8_t agc;											// the last agc received (sensor firmware V3)
	bool sensorError;										// true if received an error report (cleared by a position report)

	bool wasPrintingAtStartBit;
	bool haveStartBitData;
	bool synced;

	float extrusionCommandedThisSegment;					// the amount of extrusion commanded (mm) since we last did a comparison
	float movementMeasuredThisSegment;						// the accumulated movement in complete rotations since the previous comparison

	// Values measured for calibration
	float minMovementRatio, maxMovementRatio;
	float lastMovementRatio;
	float totalExtrusionCommanded;
	float totalMovementMeasured;

	bool dataReceived;
	bool backwards;

	enum class MagneticMonitorState : uint8_t
	{
		idle,
		calibrating,
		comparing
	};
	MagneticMonitorState magneticMonitorState;
};

#endif

#endif /* SRC_FILAMENTSENSORS_ROTATINGMAGNETFILAMENTMONITOR_H_ */
