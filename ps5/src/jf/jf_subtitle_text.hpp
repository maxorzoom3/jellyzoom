#pragma once

#include <string>

namespace jfsubtext
{

// The UI font has no glyphs for invisible bidirectional formatting marks.
inline std::string withoutFormattingMarks(const std::string &text)
{
	std::string result;
	result.reserve(text.size());
	for (std::size_t i = 0; i < text.size();) {
		const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
		std::size_t skip = 0;
		if (i + 1 < text.size() && byte(i) == 0xd8 && byte(i + 1) == 0x9c)
			skip = 2; // Arabic letter mark
		else if (i + 2 < text.size()) {
			const unsigned char a = byte(i), b = byte(i + 1), c = byte(i + 2);
			if ((a == 0xe2 && b == 0x80 && (c == 0x8b || c == 0x8e || c == 0x8f || (c >= 0xaa && c <= 0xae))) ||
			    (a == 0xe2 && b == 0x81 && c >= 0xa6 && c <= 0xa9) ||
			    (a == 0xef && b == 0xbb && c == 0xbf))
				skip = 3;
		}
		if (skip)
			i += skip;
		else
			result += text[i++];
	}
	return result;
}

}
