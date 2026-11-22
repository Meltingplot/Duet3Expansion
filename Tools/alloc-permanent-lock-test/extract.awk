# Copies the real text that the test compiles, each piece with a #line directive so that compiler messages point at the real
# source lines. Fails if a piece is missing, so a renamed function cannot silently drop out of the test. Give the files with
# absolute paths: first Duet3Expansion's src/Platform/Tasks.cpp, then CoreN2G's src/syscalls.h.
#   Tasks.cpp, to standard output: the malloc mutex, GetMallocMutex and ReleaseMallocMutex (with the RP2040 alternatives around
#              them) and Tasks::AllocPermanent
#   syscalls.h, to the file in the variable syscalls: heapTop, _sbrk and CoreAllocPermanent

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

FNR == 1 { file++; out = (file == 1) ? "/dev/stdout" : syscalls }

file == 1 && /^static Mutex mallocMutex;/ { Line(); print > out; found["mallocMutex"] = 1; next }
file == 1 && /^\/\/ We need to make malloc\/free thread safe/ { Start("GetMallocMutex", 2) }
file == 1 && /^void \*Tasks::AllocPermanent\(/ { Start("Tasks::AllocPermanent", 1) }
file == 2 && /^char \*_ecv_array heapTop = / { Line(); print > out; found["heapTop"] = 1; next }
file == 2 && /^extern "C" void \* _sbrk\(/ { Start("_sbrk", 1) }
file == 2 && /^void \*CoreAllocPermanent\(/ { Start("CoreAllocPermanent", 1) }

inFunc {
	print > out
	if ($0 ~ /^}/ && --bodies == 0) { inFunc = 0; found[name] = 1 }
}

# The piece starts here and ends with the closing brace of its n-th function
function Start(what, n)
{
	inFunc = 1
	name = what
	bodies = n
	Line()
}

function Line()
{
	printf "#line %d \"%s\"\n", FNR, FILENAME > out
}

END {
	split("mallocMutex GetMallocMutex Tasks::AllocPermanent heapTop _sbrk CoreAllocPermanent", wanted, " ")
	for (i in wanted)
	{
		if (!(wanted[i] in found)) { print wanted[i] " not found" > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
