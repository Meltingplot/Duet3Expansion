// Declarations with a known answer for pointer-check.py, see ../../README.md. "make selftest" compiles this file with the
// firmware's compiler and -g, without linking, and compares the pointers found with ../expected.txt.

struct HandleBody;
typedef HandleBody *Handle;					// like TaskHandle
typedef void (*Callback)(int) noexcept;

template <class T> class Wrapper
{
public:
	T *Get() const noexcept { return ptr; }

private:
	T *ptr = nullptr;						// found: the pointer class itself holds a raw pointer
};

struct Sample
{
	int *plain;								// found
	int *const fixed;						// not found: the pointer is const
	const int *toConst;						// found: only the int is const
	int *volatile volatilePointer;			// found
	volatile int *toVolatile;				// found
	int *array[2];							// found: an array of pointers
	int *const fixedArray[2];				// not found
	Handle handle;							// found: a typedef of a pointer
	Callback callback;						// found: a function pointer
	int& reference;							// not found: a reference cannot be reseated
	Wrapper<int> wrapped;					// not found: a class
	static Sample *list;					// found: a static member
};

template <class T> struct NeverUsed { T *never; };	// not found: a template that is never instantiated is not in the debug information

extern int value;
int *globalPointer;							// found
const char *const fixedName = "fixed";		// not found
static int *fileStatic;						// found, keyed with the file name because other files may use the same name
namespace Outer { int *inner; }				// found
namespace { int *anonymous; }				// found, keyed with the file name
Sample *Sample::list = nullptr;
auto namespaceLambda = [p = globalPointer]() noexcept { return *p; };	// not found: closure members

class Virtual								// not found: the compiler's pointer to the virtual function table has no source line
{
public:
	virtual ~Virtual() noexcept;
};

Virtual::~Virtual() noexcept { }
Sample sample{ nullptr, &value, nullptr, nullptr, nullptr, { nullptr, nullptr }, { &value, &value }, nullptr, nullptr, value, {} };

int Use(int x) noexcept
{
	static int *staticLocal = nullptr;		// not found: declared inside a function
	struct Local { int *p; } local{ &x };	// not found
	auto lambda = [&x, p = staticLocal]() noexcept { return x + ((p != nullptr) ? *p : 0); };	// not found: closure members
	staticLocal = fileStatic;
	fileStatic = anonymous;
	anonymous = Outer::inner;
	return lambda() + *local.p + ((globalPointer != nullptr) ? *globalPointer : 0) + fixedName[0]
		+ ((sample.wrapped.Get() != nullptr) ? 1 : 0) + ((Sample::list != nullptr) ? 1 : 0) + namespaceLambda();
}
