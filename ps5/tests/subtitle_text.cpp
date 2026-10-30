#include "../src/jf/jf_subtitle_text.hpp"

#include <cassert>
#include <string>

int main()
{
	const std::string lrm = "\xe2\x80\x8e";
	const std::string rlm = "\xe2\x80\x8f";
	const std::string bom = "\xef\xbb\xbf";
	assert(jfsubtext::withoutFormattingMarks(lrm + "First\n" + lrm + "Second") == "First\nSecond");
	assert(jfsubtext::withoutFormattingMarks(bom + rlm + "Real?") == "Real?");
	assert(jfsubtext::withoutFormattingMarks("\xe2\x81\xa6" "word" "\xe2\x81\xa9") == "word");
	assert(jfsubtext::withoutFormattingMarks("\xe2\x80\xaa" "word" "\xe2\x80\xac") == "word");
	assert(jfsubtext::withoutFormattingMarks("\xd8\x9c" "word") == "word");
	assert(jfsubtext::withoutFormattingMarks("\xc3\xa9" " ? " "\xe2\x99\xaa") == "\xc3\xa9" " ? " "\xe2\x99\xaa");
	assert(jfsubtext::withoutFormattingMarks("\xe2\x80") == "\xe2\x80");
	assert(jfsubtext::withoutFormattingMarks("") == "");
}
