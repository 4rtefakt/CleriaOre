// The text the loader's dinput8.dll carries, so the manager tells it from another dinput8.dll.
#pragma once

namespace ore {

inline constexpr char kLoaderMark[] = "CleriaOre loader";
inline constexpr char kVersion[] = "1.0.0";   // res/cleriaore.rc says the same

}  // namespace ore
