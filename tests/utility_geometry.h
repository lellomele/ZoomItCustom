#pragma once
#include <limits>

inline void RunUtilityGeometryTests()
{
    const LONG lowest = (std::numeric_limits<LONG>::min)();
    const LONG highest = (std::numeric_limits<LONG>::max)();
    auto clipped = ForceRectInBounds(RECT{-500,-500,1500,1500}, RECT{10,20,110,220});
    require(clipped.left==10 && clipped.top==20 && clipped.right==110 && clipped.bottom==220,
            "Oversized selection must fit entirely inside bounds");
    clipped = ForceRectInBounds(RECT{lowest,lowest,highest,highest}, RECT{0,0,100,200});
    require(clipped.left==0 && clipped.top==0 && clipped.right==100 && clipped.bottom==200,
            "Extreme rectangle must not overflow when clipped");
    clipped = ForceRectInBounds(RECT{10,20,5,6}, RECT{0,0,100,200});
    require(clipped.left==clipped.right && clipped.top==clipped.bottom,
            "Inverted selection must become empty");
    auto selected = RectFromPointsMinSize(POINT{highest,highest}, POINT{highest,highest},34);
    require(selected.right==highest && selected.bottom==highest &&
            static_cast<int64_t>(selected.right)-selected.left==34 &&
            static_cast<int64_t>(selected.bottom)-selected.top==34,
            "Selection at maximum coordinate must retain minimum dimensions");
    selected = RectFromPointsMinSize(POINT{lowest,highest}, POINT{highest,lowest},34);
    require(selected.left==lowest && selected.top==lowest && selected.right==highest && selected.bottom==highest,
            "Selection across integer limits must saturate safely");
    selected = RectFromPointsMinSize(POINT{5,7}, POINT{5,7},-1);
    require(selected.left==5 && selected.right==6 && selected.top==7 && selected.bottom==8,
            "Negative minimum size must not invert selection");
    const RECT full{lowest,lowest,highest,highest};
    auto mapped = ScalePointInRects(POINT{highest,lowest}, RECT{lowest,lowest,lowest+1,lowest+1}, full);
    require(mapped.x==highest && mapped.y==-1,
            "Out-of-range mapped coordinate must saturate without overflow");
    mapped = ScalePointInRects(POINT{10,20}, RECT{0,0,0,0}, RECT{5,7,20,30});
    require(mapped.x==5 && mapped.y==7,"Empty source must safely map to target origin");
    mapped = ScalePointInRects(POINT{10,20}, RECT{0,0,20,30}, RECT{5,7,5,7});
    require(mapped.x==5 && mapped.y==7,"Empty target must safely map to target origin");
    for (LONG coordinate=-500;coordinate<=500;++coordinate) {
        const RECT source{-123,-231,377,769}, target{-299,101,1501,1301};
        mapped=ScalePointInRects(POINT{coordinate,coordinate},source,target);
        require(mapped.x==601+MulDiv(coordinate-127,1800,500) &&
                mapped.y==701+MulDiv(coordinate-269,1200,1000),
                "Ordinary coordinate mapping must preserve Win32 rounding");
    }
    require(GetDpiForWindowHelper(nullptr)>0,"Invalid HWND must yield usable DPI fallback");
}
