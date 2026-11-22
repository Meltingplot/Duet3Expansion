// A translation unit that declares no reseatable raw pointer. "make selftest" runs "pointer-check.py --update" on it, which
// must write an allowlist with the header and no entries.

int valueWithoutPointer = 0;
int *const pointerThatCannotBeReseated = &valueWithoutPointer;
