# Copies function definitions, class or struct definitions, or the case of a switch over SbcRequest out of a source file, each with a
# #line directive, so that the test compiles the real text and compiler messages point at the real source lines. Fails if one of them is
# missing, so a renamed function cannot silently drop out of the test.
#
#   awk -v f=<path for #line> -v names="Class::Function ..." -f extract.awk <file>	function definitions, from the head to the "}" in column 1
#   awk -v f=<path for #line> -v names="class:Name ..." -f extract.awk <file>		"class Name", "class Name : <bases>", "struct Name" or "template<...> class Name" to "};"
#   awk -v f=<path for #line> -v names="case:SetVariable" -f extract.awk <file>		"case SbcRequest::SetVariable:" and its block

{ sub(/\r$/, "") }		# the Windows checkout has CRLF line endings

BEGIN { numWanted = split(names, wanted, " ") }

inBlock {
	print
	if ($0 ~ endPattern) { inBlock = 0; found[current] = 1 }
	next
}

{
	head = $0
	sub(/^\/\*static\*\/ /, "", head)
	for (i = 1; i <= numWanted; i++)
	{
		w = wanted[i]
		if (w ~ /^case:/)
		{
			if ($0 == "\t\tcase SbcRequest::" substr(w, 6) ":") { current = w; endPattern = "^\t\t}$" }
			else { continue }
		}
		else if (w ~ /^class:/)
		{
			n = substr(w, 7)
			if ($0 == "class " n || $0 == "struct " n || $0 ~ ("^class " n " : ") || $0 ~ ("^template<[^>]*> class " n "$")) { current = w; endPattern = "^};" }
			else { continue }
		}
		else if (head ~ /^[A-Za-z]/ && index(head, w "(") > 0 && head !~ /;[ \t]*$/)
		{
			current = w; endPattern = "^}"
		}
		else { continue }
		inBlock = 1
		printf "#line %d \"%s\"\n", NR, f
		print
		break
	}
}

END {
	for (i = 1; i <= numWanted; i++)
	{
		if (!(wanted[i] in found)) { print wanted[i] " not found in " f > "/dev/stderr"; missing = 1 }
	}
	if (missing) { exit 1 }
}
