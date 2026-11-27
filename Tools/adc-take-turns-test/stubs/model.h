// Host stand-ins for the headers that CoreN2G src/SAME5x_C21/SAMC21/AnalogIn.cpp includes. Only what that file uses is declared.
// The ADC, the SDADC and the DMAC behind these declarations are modelled in adc_take_turns_test.cpp.
#ifndef ADC_TAKE_TURNS_TEST_MODEL_H
#define ADC_TAKE_TURNS_TEST_MODEL_H

#include <cstdint>
#include <cstddef>
#include <atomic>

#define SAMC21			1

#define ARRAY_SIZE(_x)	(sizeof(_x)/sizeof((_x)[0]))

typedef uint8_t DmaChannel;
typedef uint8_t DmaPriority;
typedef uint32_t NvicPriority;

// CoreN2G src/CoreIO.h
union CallbackParameter
{
	void *vp;
	uint32_t u32;
	int32_t i32;

	explicit CallbackParameter(void *pp) noexcept : vp(pp) { }
	explicit CallbackParameter(uint32_t pp) noexcept : u32(pp) { }
	explicit CallbackParameter(int32_t pp) noexcept : i32(pp) { }
	CallbackParameter() noexcept : u32(0) { }
};

// CoreN2G src/CoreIO.h, the SAMC21 part
enum class AdcInput : uint8_t
{
	adc0_0 = 0x00, adc0_1, adc0_2, adc0_3, adc0_4, adc0_5, adc0_6, adc0_7, adc0_8, adc0_9, adc0_10, adc0_11,
	sdadc_0 = 0x10, sdadc_1,
	none = 0xFF
};
typedef AdcInput AnalogChannelNumber;
static inline constexpr unsigned int GetDeviceNumber(AdcInput ain) noexcept { return (uint8_t)ain >> 4; }
static inline constexpr unsigned int GetInputNumber(AdcInput ain) noexcept { return (uint8_t)ain & 0x0F; }

constexpr unsigned int GclkNum48MHz = 0;

uint32_t millis() noexcept;
void delay(uint32_t ms) noexcept;

// Model hooks called by the register stand-ins below
struct ModelConverter;
void ModelSoftwareTrigger(ModelConverter *conv) noexcept;
void ModelSoftwareReset(ModelConverter *conv) noexcept;

// A write to SWTRIG starts a conversion
struct SwtrigReg
{
	ModelConverter *conv;
	SwtrigReg& operator=(uint32_t val) noexcept { if (val != 0) { ModelSoftwareTrigger(conv); } return *this; }
};

struct Adc
{
	ModelConverter *conv;
	struct { volatile uint16_t reg; } RESULT;
	struct { volatile uint32_t reg; } SEQCTRL;
	struct { SwtrigReg reg; } SWTRIG;
	uint32_t ctrla;
};

struct Sdadc
{
	ModelConverter *conv;
	struct { volatile uint32_t reg; } RESULT;
	struct { volatile uint32_t reg; } SEQCTRL;
	struct { SwtrigReg reg; } SWTRIG;
	uint32_t ctrla;
};

struct Tsens
{
	struct { uint32_t reg; struct { uint32_t SWRST : 1, ENABLE : 1; } bit; } CTRLA;
	struct { uint32_t reg; struct { uint32_t START : 1; } bit; } CTRLB;
	struct { uint32_t reg; } CTRLC;
	struct { struct { uint32_t RESRDY : 1; } bit; } INTFLAG;
	struct { struct { uint32_t VALUE : 24; } bit; } VALUE;
	struct { uint32_t reg; } GAIN, OFFSET, CAL;
	struct { struct { uint32_t ENABLE : 1; } bit; } SYNCBUSY;
};

extern Adc modelAdc0, modelAdc1;
extern Sdadc modelSdadc;
extern Tsens modelTsens;
extern uint32_t modelFuseWord;
#define ADC0		(&modelAdc0)
#define ADC1		(&modelAdc1)
#define SDADC		(&modelSdadc)
#define TSENS		(&modelTsens)
#define MCLK		nullptr
#define GCLK		nullptr

// Register values. These are placeholders, not the values of the SAMC21 headers: the model reacts only to the SWRST bit of CTRLA and to a
// non-zero write to SWTRIG, the others just have to exist.
#define ADC_CTRLA_SWRST					(1u << 0)
#define ADC_CTRLA_ENABLE				(1u << 1)
#define ADC_SYNCBUSY_SWRST				(1u << 0)
#define ADC_SYNCBUSY_ENABLE				(1u << 1)
#define ADC_SWTRIG_START				(1u << 1)
#define ADC_CTRLB_PRESCALER_DIV4		1u
#define ADC_CTRLC_RESSEL_16BIT			(1u << 4)
#define ADC_CTRLC_R2R					(1u << 7)
#define ADC_REFCTRL_REFSEL_AREFA		3u
#define ADC_REFCTRL_REFSEL_INTVCC2		5u
#define ADC_EVCTRL_RESRDYEO				(1u << 4)
#define ADC_INPUTCTRL_MUXNEG_Pos		8
#define ADC_AVGCTRL_SAMPLENUM_64		6u
#define ADC_SAMPCTRL_OFFCOMP			(1u << 7)
#define ADC_CALIB_BIASCOMP(_x)			((uint32_t)(_x))
#define ADC_CALIB_BIASREFBUF(_x)		((uint32_t)(_x) << 8)
#define ADC0_FUSES_BIASCOMP_ADDR		((uintptr_t)&modelFuseWord)
#define ADC0_FUSES_BIASCOMP_Msk			7u
#define ADC0_FUSES_BIASCOMP_Pos			0
#define ADC0_FUSES_BIASREFBUF_ADDR		((uintptr_t)&modelFuseWord)
#define ADC0_FUSES_BIASREFBUF_Msk		7u
#define ADC0_FUSES_BIASREFBUF_Pos		0
#define ADC1_FUSES_BIASCOMP_ADDR		((uintptr_t)&modelFuseWord)
#define ADC1_FUSES_BIASCOMP_Msk			7u
#define ADC1_FUSES_BIASCOMP_Pos			0
#define ADC1_FUSES_BIASREFBUF_ADDR		((uintptr_t)&modelFuseWord)
#define ADC1_FUSES_BIASREFBUF_Msk		7u
#define ADC1_FUSES_BIASREFBUF_Pos		0
#define SDADC_CTRLA_SWRST				(1u << 0)
#define SDADC_CTRLA_ENABLE				(1u << 1)
#define SDADC_SYNCBUSY_SWRST			(1u << 0)
#define SDADC_SYNCBUSY_ENABLE			(1u << 1)
#define SDADC_SWTRIG_START				(1u << 1)
#define SDADC_CTRLB_PRESCALER(_x)		((uint32_t)(_x))
#define SDADC_CTRLB_OSR(_x)				((uint32_t)(_x) << 8)
#define SDADC_CTRLB_SKPCNT(_x)			((uint32_t)(_x) << 12)
#define SDADC_REFCTRL_REFSEL_INTVCC		1u
#define SDADC_REFCTRL_REFRANGE(_x)		((uint32_t)(_x) << 4)
#define SDADC_EVCTRL_RESRDYEO			(1u << 4)
#define TSENS_CTRLB_START				(1u << 0)
#define TSENS_CAL_TCAL(_x)				((uint32_t)(_x))
#define TSENS_CAL_FCAL(_x)				((uint32_t)(_x) << 8)
#define TSENS_FUSES_GAIN_0_ADDR			((uintptr_t)&modelFuseWord)
#define TSENS_FUSES_GAIN_0_Msk			0xFFu
#define TSENS_FUSES_GAIN_0_Pos			0
#define TSENS_FUSES_GAIN_1_ADDR			((uintptr_t)&modelFuseWord)
#define TSENS_FUSES_GAIN_1_Msk			0xFFu
#define TSENS_FUSES_GAIN_1_Pos			0
#define TSENS_FUSES_OFFSET_ADDR			((uintptr_t)&modelFuseWord)
#define TSENS_FUSES_OFFSET_Msk			0xFFu
#define TSENS_FUSES_OFFSET_Pos			0
#define TSENS_FUSES_FCAL_ADDR			((uintptr_t)&modelFuseWord)
#define TSENS_FUSES_FCAL_Msk			0xFFu
#define TSENS_FUSES_FCAL_Pos			0
#define TSENS_FUSES_TCAL_ADDR			((uintptr_t)&modelFuseWord)
#define TSENS_FUSES_TCAL_Msk			0xFFu
#define TSENS_FUSES_TCAL_Pos			0
#define DMAC_BTCTRL_VALID				(1u << 0)
#define DMAC_BTCTRL_EVOSEL_DISABLE		0u
#define DMAC_BTCTRL_BLOCKACT_INT		(1u << 3)
#define DMAC_BTCTRL_BEATSIZE_HWORD		(1u << 8)
#define DMAC_BTCTRL_DSTINC				(1u << 11)
#define DMAC_BTCTRL_STEPSEL_DST			0u
#define DMAC_BTCTRL_STEPSIZE_X1			0u
#define DMAC_CHINTFLAG_TERR				(1u << 0)
#define DMAC_CHINTFLAG_TCMPL			(1u << 1)
#define GCLK_PCHCTRL_GEN(_x)			((uint32_t)(_x))
#define GCLK_PCHCTRL_CHEN				(1u << 6)
#define GCLK_PCHCTRL_CHEN_Pos			6
#define GCLK_PCHCTRL_GEN_GCLK0_Val		0u
#define ADC0_GCLK_ID					33
#define SDADC_GCLK_ID					35
#define TSENS_GCLK_ID					5

// hri functions: a software reset stops a conversion, everything else is accepted and ignored
template<class D> inline bool hri_is_syncing(D *, uint32_t) noexcept { return false; }
inline bool hri_adc_is_syncing(Adc *d, uint32_t m) noexcept { return hri_is_syncing(d, m); }
inline void hri_adc_wait_for_sync(Adc *, uint32_t) noexcept { }
inline uint32_t hri_adc_get_CTRLA_reg(Adc *d, uint32_t m) noexcept { return d->ctrla & m; }
inline void hri_adc_clear_CTRLA_ENABLE_bit(Adc *d) noexcept { d->ctrla &= ~ADC_CTRLA_ENABLE; }
inline void hri_adc_set_CTRLA_ENABLE_bit(Adc *d) noexcept { d->ctrla |= ADC_CTRLA_ENABLE; }
inline void hri_adc_write_CTRLA_reg(Adc *d, uint32_t v) noexcept { if (v & ADC_CTRLA_SWRST) { d->ctrla = 0; ModelSoftwareReset(d->conv); } else { d->ctrla = v; } }
inline void hri_adc_write_CTRLB_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_CTRLC_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_REFCTRL_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_EVCTRL_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_INPUTCTRL_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_AVGCTRL_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_SAMPCTRL_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_WINLT_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_WINUT_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_GAINCORR_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_OFFSETCORR_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_DBGCTRL_reg(Adc *, uint32_t) noexcept { }
inline void hri_adc_write_CALIB_reg(Adc *, uint32_t) noexcept { }
inline bool hri_sdadc_is_syncing(Sdadc *d, uint32_t m) noexcept { return hri_is_syncing(d, m); }
inline void hri_sdadc_wait_for_sync(Sdadc *, uint32_t) noexcept { }
inline uint32_t hri_sdadc_get_CTRLA_reg(Sdadc *d, uint32_t m) noexcept { return d->ctrla & m; }
inline void hri_sdadc_clear_CTRLA_ENABLE_bit(Sdadc *d) noexcept { d->ctrla &= ~SDADC_CTRLA_ENABLE; }
inline void hri_sdadc_set_CTRLA_ENABLE_bit(Sdadc *d) noexcept { d->ctrla |= SDADC_CTRLA_ENABLE; }
inline void hri_sdadc_write_CTRLA_reg(Sdadc *d, uint32_t v) noexcept { if (v & SDADC_CTRLA_SWRST) { d->ctrla = 0; ModelSoftwareReset(d->conv); } else { d->ctrla = v; } }
inline void hri_sdadc_write_CTRLB_reg(Sdadc *, uint32_t) noexcept { }
inline void hri_sdadc_write_REFCTRL_reg(Sdadc *, uint32_t) noexcept { }
inline void hri_sdadc_write_EVCTRL_reg(Sdadc *, uint32_t) noexcept { }
inline void hri_sdadc_write_WINLT_reg(Sdadc *, uint32_t) noexcept { }
inline void hri_sdadc_write_WINUT_reg(Sdadc *, uint32_t) noexcept { }
inline void hri_sdadc_write_GAINCORR_reg(Sdadc *, uint32_t) noexcept { }
inline void hri_sdadc_write_OFFSETCORR_reg(Sdadc *, uint32_t) noexcept { }
inline void hri_sdadc_write_SHIFTCORR_reg(Sdadc *, uint32_t) noexcept { }
inline void hri_sdadc_set_ANACTRL_ONCHOP_bit(Sdadc *) noexcept { }
inline void hri_sdadc_write_DBGCTRL_reg(Sdadc *, uint32_t) noexcept { }
inline void hri_mclk_set_APBCMASK_ADC0_bit(void *) noexcept { }
inline void hri_mclk_set_APBCMASK_SDADC_bit(void *) noexcept { }
inline void hri_mclk_set_APBAMASK_TSENS_bit(void *) noexcept { }
inline void hri_gclk_write_PCHCTRL_reg(void *, uint32_t, uint32_t) noexcept { }

// CoreN2G src/DmacManager.h
enum class DmaCallbackReason : uint8_t
{
	none = 0,
	error = DMAC_CHINTFLAG_TERR,
	complete = DMAC_CHINTFLAG_TCMPL,
	completeAndError = DMAC_CHINTFLAG_TERR | DMAC_CHINTFLAG_TCMPL,
};
typedef void (*DmaCallbackFunction)(CallbackParameter cb, DmaCallbackReason reason) noexcept;
enum class DmaTrigSource : uint8_t { adc0_resrdy = 1, adc1_resrdy, sdadc_resrdy };		// placeholder values, the model only tells them apart

namespace DmacManager
{
	void SetBtctrl(DmaChannel channel, uint32_t val) noexcept;
	void SetSourceAddress(DmaChannel channel, const volatile void *const src) noexcept;
	void SetDestinationAddress(DmaChannel channel, volatile void *const dst) noexcept;
	void SetDataLength(DmaChannel channel, uint32_t amount) noexcept;
	void SetTriggerSource(DmaChannel channel, DmaTrigSource source) noexcept;
	void EnableChannel(DmaChannel channel, DmaPriority priority) noexcept;
	bool DisableChannel(DmaChannel channel) noexcept;
	void SetInterruptCallback(DmaChannel channel, DmaCallbackFunction fn, CallbackParameter param) noexcept;
	void EnableCompletedInterrupt(DmaChannel channel) noexcept;
	void DisableCompletedInterrupt(DmaChannel channel) noexcept;
	uint32_t GetAndClearChannelStatus(DmaChannel channel) noexcept;
}

// CoreN2G src/Cache.h: the SAMC21 has no cache
namespace Cache
{
	inline void InvalidateAfterDMAReceive(const volatile void *, size_t) noexcept { }
}

// CoreN2G src/CoreNotifyIndices.h
namespace NotifyIndices
{
	constexpr uint32_t AnalogIn = 1;
}

// RRFLibraries src/RTOSIface/RTOSIface.h
typedef void *TaskHandle;
class TaskBase
{
public:
	static TaskHandle GetCallerTaskHandle() noexcept;
	static void GiveFromISR(TaskHandle h, uint32_t index) noexcept;
	static bool TakeIndexed(uint32_t index, uint32_t timeout) noexcept;
	static void Yield() noexcept;
};
class TaskCriticalSectionLocker { public: TaskCriticalSectionLocker() noexcept { } };
class AtomicCriticalSectionLocker { public: AtomicCriticalSectionLocker() noexcept { } };

// RRFLibraries src/RTOSIface/RTOSIface.h. Take and Release are modelled in adc_take_turns_test.cpp, where the TMC task competes for the mutex.
class Mutex
{
public:
	void Create(const char *) noexcept { }
	bool Take(uint32_t timeout = TimeoutUnlimited) noexcept;
	bool Release() noexcept;

	static constexpr uint32_t TimeoutUnlimited = 0xFFFFFFFFu;
};

class MutexLocker
{
public:
	explicit MutexLocker(Mutex& m, uint32_t timeout = Mutex::TimeoutUnlimited) noexcept : handle(&m), acquired(m.Take(timeout)) { }
	~MutexLocker() { Release(); }
	void Release() noexcept { if (acquired) { handle->Release(); acquired = false; } }

	MutexLocker(const MutexLocker&) = delete;
	MutexLocker& operator=(const MutexLocker&) = delete;

private:
	Mutex *handle;
	bool acquired;
};

// CoreN2G src/DmacManager.h, SAMC21 only
namespace DmacManager
{
	Mutex& GetTransferMutex() noexcept;
}

#endif
