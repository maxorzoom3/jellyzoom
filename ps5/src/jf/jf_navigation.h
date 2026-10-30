#pragma once

namespace jf
{

inline int sectionShortcut(bool overlayOpen, bool detailsOpen, bool browsing, bool transitioning,
	bool previous, bool next)
{
	if (overlayOpen || detailsOpen || browsing || transitioning || previous == next)
		return 0;
	return next ? 1 : -1;
}

enum class SectionDestination { none, section, discover, profile };

struct SectionStep
{
	SectionDestination destination;
	int index;
};

inline SectionStep sectionStep(int count, int current, bool discoverEnabled, bool onDiscover,
	bool onProfile, int direction)
{
	if (count <= 0 || (direction != -1 && direction != 1))
		return { SectionDestination::none, -1 };
	if (onProfile)
		return direction < 0 ? SectionStep{ discoverEnabled ? SectionDestination::discover : SectionDestination::section, count - 1 }
		                     : SectionStep{ SectionDestination::none, -1 };
	if (onDiscover)
		return direction < 0 ? SectionStep{ SectionDestination::section, count - 1 }
		                     : SectionStep{ SectionDestination::profile, -1 };
	if (current < 0 || current >= count)
		return { SectionDestination::none, -1 };
	if (direction < 0 && current == 0)
		return { discoverEnabled ? SectionDestination::discover : SectionDestination::none, -1 };
	if (direction > 0 && current == count - 1)
		return { discoverEnabled ? SectionDestination::discover : SectionDestination::profile, -1 };
	return { SectionDestination::section, current + direction };
}

}
