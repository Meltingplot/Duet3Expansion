/*
 * AveragingFilter.h
 *
 *  Created on: 7 Sep 2018
 *      Author: David
 */

#ifndef SRC_PLATFORM_AVERAGINGFILTER_H_
#define SRC_PLATFORM_AVERAGINGFILTER_H_

#include "RepRapFirmware.h"
#include "RTOSIface/RTOSIface.h"

// Class to perform averaging of values read from the ADC
// numAveraged should be a power of 2 for best efficiency
template<size_t numAveraged> class AveragingFilter
{
public:
	AveragingFilter() noexcept
	{
		Init(0);
	}

	void Init(uint16_t val) volatile noexcept
	{
		TaskCriticalSectionLocker lock;

		sum = (uint32_t)val * (uint32_t)numAveraged;
		index = 0;
		isValid = false;
		for (size_t i = 0; i < numAveraged; ++i)
		{
			readings[i] = val;
		}
	}

	// Call this to put a new reading into the filter
	void ProcessReading(uint16_t r) noexcept
	{
		TaskCriticalSectionLocker lock;

		sum = sum - readings[index] + r;
		readings[index] = r;
		++index;
		if (index == numAveraged)
		{
			index = 0;
			isValid = true;
		}
	}

	// Return the raw sum
	uint32_t GetSum() const volatile noexcept
	{
		return sum;
	}

	// Return true if we have a valid average
	bool IsValid() const volatile noexcept
	{
		return isValid;
	}

	// Get the latest reading
	uint16_t GetLatestReading() const volatile noexcept
	{
		size_t indexOfLastReading = index;			// capture volatile variable
		indexOfLastReading = (indexOfLastReading == 0) ? numAveraged - 1 : indexOfLastReading - 1;
		return readings[indexOfLastReading];
	}

	static constexpr size_t NumAveraged() noexcept { return numAveraged; }

	// Function used as an ADC callback to feed a result into an averaging filter
	static void CallbackFeedIntoFilter(CallbackParameter cp, uint32_t val) noexcept;

	bool CheckIntegrity() const noexcept;

private:
	// These fields are written by the ADC callback (ISR or high-priority task) and read by other tasks.
	// They MUST be declared volatile: the member functions above are volatile-qualified, but the filter
	// objects themselves are plain (non-volatile) statics. With -O3 the compiler is therefore free to keep
	// 'sum' in a register and to reorder the stores in ProcessReading across the ISR boundary. The result
	// was that thermistor / Vref / Vssa readings could be cached at a stale (too low) value and only jump to
	// the correct value under interrupt load. Declaring the storage volatile forces a real memory access on
	// every read and write at all optimisation levels. This is the permanent equivalent of the work-arounds
	// that "healed" the bug: printing Vref/Vssa from Spin (which forces a volatile read), or building at -O2.
	volatile uint16_t readings[numAveraged];
	volatile size_t index;
	volatile uint32_t sum;
	volatile bool isValid;
	//invariant(sum == + over readings)
	//invariant(index < numAveraged)
};

// This is called from an ISR or high priority task to add a new reading to the filter.
template<size_t numAveraged> void AveragingFilter<numAveraged>::CallbackFeedIntoFilter(CallbackParameter cp, uint32_t val) noexcept
{
	static_cast<AveragingFilter<numAveraged>*>(cp.vp)->ProcessReading((uint16_t)val);
}

template<size_t numAveraged> bool AveragingFilter<numAveraged>::CheckIntegrity() const noexcept
{
	AtomicCriticalSectionLocker lock;

	uint32_t locSum = 0;
	for (size_t i = 0; i < numAveraged; ++i)
	{
		locSum += readings[i];
	}
	return locSum == sum;
}

#endif /* SRC_PLATFORM_AVERAGINGFILTER_H_ */
