#include "../src/Zoomit.cpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <numeric>
#include <psapi.h>

namespace reference {
#include "reference_drawing_effects.h"
}

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TestCanvas {
    HDC dc_{};
    HBITMAP bitmap_{};
    BYTE* pixels_{};
public:
    static constexpr int Width=800, Height=500;
    TestCanvas() {
        BITMAPINFO info{};
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=Width; info.bmiHeader.biHeight=-Height;
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
        info.bmiHeader.biCompression=BI_RGB;
        dc_=CreateCompatibleDC(nullptr);
        void* bits{};
        bitmap_=CreateDIBSection(dc_,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        check(dc_ && bitmap_ && bits,"Create patterned DIB");
        check(SelectObject(dc_,bitmap_)!=nullptr,"Select patterned DIB");
        pixels_=static_cast<BYTE*>(bits);
        reset();
    }
    ~TestCanvas() { if(dc_) DeleteDC(dc_); if(bitmap_) DeleteObject(bitmap_); }
    HDC dc() const { return dc_; }
    const BYTE* pixels() const { return pixels_; }
    void reset(unsigned seed=7) {
        GdiFlush();
        for(int y=0;y<Height;++y) for(int x=0;x<Width;++x) {
            auto* p=pixels_+(static_cast<size_t>(y)*Width+x)*4;
            p[0]=static_cast<BYTE>((x*17+y*3+seed)%256);
            p[1]=static_cast<BYTE>((x*5+y*13+seed*3)%256);
            p[2]=static_cast<BYTE>((x*11+y*7+seed*5)%256);
            p[3]=static_cast<BYTE>((x+y)%3==0?0:(x+y)%3==1?128:255);
        }
    }
};

static size_t compareRgb(const TestCanvas& oldCanvas, const TestCanvas& newCanvas) {
    GdiFlush();
    for(size_t i=0;i<static_cast<size_t>(TestCanvas::Width)*TestCanvas::Height;++i)
        for(int channel=0;channel<3;++channel)
            if(oldCanvas.pixels()[i*4+channel]!=newCanvas.pixels()[i*4+channel]) {
                std::cerr<<"Different pixel "<<i<<" channel "<<channel<<" old "
                         <<static_cast<unsigned>(oldCanvas.pixels()[i*4+channel])<<" new "
                         <<static_cast<unsigned>(newCanvas.pixels()[i*4+channel])<<"\n";
                throw std::runtime_error("Optimized effect changes RGB output");
            }
    return static_cast<size_t>(TestCanvas::Width)*TestCanvas::Height;
}

struct Result { const char* name; double oldMs; double newMs; int operations; };

template<class OldOperation,class NewOperation>
Result measure(const char* name,TestCanvas& canvas,int operations,OldOperation oldOperation,NewOperation newOperation) {
    for(int i=0;i<8;++i) { oldOperation(); newOperation(); }
    std::vector<double> oldTimes,newTimes;
    auto timed=[&](auto operation) {
        canvas.reset(); GdiFlush();
        const auto start=std::chrono::steady_clock::now();
        for(int i=0;i<operations;++i) operation();
        GdiFlush();
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    };
    for(int trial=0;trial<9;++trial) {
        if(trial%2) { newTimes.push_back(timed(newOperation)); oldTimes.push_back(timed(oldOperation)); }
        else { oldTimes.push_back(timed(oldOperation)); newTimes.push_back(timed(newOperation)); }
    }
    std::sort(oldTimes.begin(),oldTimes.end()); std::sort(newTimes.begin(),newTimes.end());
    return {name,oldTimes[oldTimes.size()/2],newTimes[newTimes.size()/2],operations};
}

int main() {
    ULONG_PTR token{};
    try {
        Gdiplus::GdiplusStartupInput startup;
        check(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"GDI+ startup");
        std::vector<Result> results;
        size_t cases{},pixelsCompared{},rejectedCases{};
        DWORD gdiBefore{},gdiAfter{};
        {
            TestCanvas oldCanvas,newCanvas,background;
            g_PenWidth=9; g_BlurRadius=3;
            Gdiplus::Pen pen(Gdiplus::Color(255,255,255,0),9);
            pen.SetLineCap(Gdiplus::LineCapRound,Gdiplus::LineCapRound,Gdiplus::DashCapRound);
            const RECT rectangles[]{ {10,10,26,22},{20,20,84,68},{40,40,680,400},
                                     {-8,-7,60,55},{760,470,840,540},{0,0,800,500} };
            const DWORD colors[]{0x8000ffff,0x80ff0000,0x80123456,0x00000000,0xffabcdef};
            const DWORD shapes[]{DRAW_RECTANGLE,DRAW_ELLIPSE,DRAW_LINE};
            for(DWORD color:colors) {
                g_PenColor=color;
                Gdiplus::SolidBrush brush(ColorFromColorRef(color));
                for(bool blur:{false,true}) for(DWORD shape:shapes) for(const RECT& rect:rectangles) {
                    oldCanvas.reset(); newCanvas.reset();
                    reference::DrawEffectShape(shape,oldCanvas.dc(),&brush,&pen,rect.left,rect.top,rect.right,rect.bottom,blur);
                    DrawEffectShape(shape,newCanvas.dc(),&brush,&pen,rect.left,rect.top,rect.right,rect.bottom,blur);
                    pixelsCompared+=compareRgb(oldCanvas,newCanvas); ++cases;
                }
            }
            g_PenColor=0x8000ffff;
            const Gdiplus::Rect lineBounds[]{ {10,10,90,45},{40,40,640,360},{-8,-8,100,50} };
            for(bool separate:{false,true}) for(bool blur:{false,true}) for(const auto& bounds:lineBounds) {
                oldCanvas.reset(); newCanvas.reset(); background.reset(55);
                const POINT from{bounds.X+7,bounds.Y+7},to{bounds.X+bounds.Width-8,bounds.Y+bounds.Height-8};
                for(int stroke=0;stroke<3;++stroke) {
                    reference::DrawMaskedLine(oldCanvas.dc(),separate?background.dc():oldCanvas.dc(),bounds,from,to,&pen,blur);
                    DrawMaskedLine(newCanvas.dc(),separate?background.dc():newCanvas.dc(),bounds,from,to,&pen,blur);
                    pixelsCompared+=compareRgb(oldCanvas,newCanvas); ++cases;
                }
            }
            // A sparse mask with fractional alpha must retain the existing nonzero-alpha rule.
            for(bool separate:{false,true}) {
                constexpr int width=65,height=33;
                std::vector<BYTE> maskPixels(width*height*4);
                for(size_t i=0;i<maskPixels.size()/4;++i) maskPixels[i*4+3]=static_cast<BYTE>(i%5==0?0:i%5==1?1:i%5==2?127:i%5==3?128:255);
                Gdiplus::Bitmap mask(width,height,width*4,PixelFormat32bppARGB,maskPixels.data());
                oldCanvas.reset(); newCanvas.reset(); background.reset(55);
                check(reference::PaintDrawingMask(oldCanvas.dc(),separate?background.dc():oldCanvas.dc(),{20,20,width,height},mask,false),"Reference sparse mask");
                check(PaintDrawingMask(newCanvas.dc(),separate?background.dc():newCanvas.dc(),{20,20,width,height},mask,false),"Optimized sparse mask");
                pixelsCompared+=compareRgb(oldCanvas,newCanvas); ++cases;
                newCanvas.reset(); oldCanvas.reset();
                check(!PaintDrawingMask(newCanvas.dc(),newCanvas.dc(),{20,20,width+1,height},mask,false),"Mismatched mask must fail safely");
                pixelsCompared+=compareRgb(oldCanvas,newCanvas); ++cases;
            }
            Gdiplus::SolidBrush brush(Gdiplus::Color(128,255,255,0));
            const int minimum=(std::numeric_limits<INT>::min)(), maximum=(std::numeric_limits<INT>::max)();
            const DWORD invalidGdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            auto rejected=[&](auto operation) {
                oldCanvas.reset(); newCanvas.reset();
                for(int repeat=0;repeat<25;++repeat) operation();
                pixelsCompared+=compareRgb(oldCanvas,newCanvas); ++rejectedCases;
                check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==invalidGdiBefore,
                      "Rejected extreme input must not leak GDI resources");
            };
            // The old reference intentionally never receives overflowing coordinates.
            rejected([&]{DrawEffectShape(DRAW_RECTANGLE,newCanvas.dc(),&brush,&pen,minimum,20,maximum,40,false);});
            rejected([&]{DrawEffectShape(DRAW_ELLIPSE,newCanvas.dc(),&brush,&pen,20,minimum,40,maximum,true);});
            rejected([&]{DrawEffectShape(DRAW_LINE,newCanvas.dc(),&brush,&pen,minimum,minimum,maximum,maximum,false);});
            rejected([&]{DrawEffectShape(DRAW_LINE,newCanvas.dc(),&brush,&pen,minimum,20,minimum+30,50,false);});
            rejected([&]{DrawEffectShape(DRAW_LINE,newCanvas.dc(),&brush,&pen,maximum-30,20,maximum,50,true);});
            rejected([&]{const DWORD previous=g_PenWidth; g_PenWidth=(std::numeric_limits<DWORD>::max)();
                         DrawEffectShape(DRAW_LINE,newCanvas.dc(),&brush,&pen,20,20,80,60,false); g_PenWidth=previous;});
            rejected([&]{DrawMaskedLine(newCanvas.dc(),newCanvas.dc(),{minimum,20,100,50},
                                       {maximum,25},{minimum+20,50},&pen,false);});
            rejected([&]{DrawMaskedLine(newCanvas.dc(),newCanvas.dc(),{maximum-100,20,100,50},
                                       {minimum,25},{maximum-20,50},&pen,true);});
            rejected([&]{DrawMaskedLine(newCanvas.dc(),newCanvas.dc(),{20,20,65536,65536},
                                       {25,25},{50,50},&pen,false);});
            rejected([&]{DrawMaskedLine(newCanvas.dc(),newCanvas.dc(),{20,20,100,50},
                                       {25,25},{50,50},nullptr,false);});
            rejected([&]{DrawHighlightedShape(DRAW_RECTANGLE,newCanvas.dc(),nullptr,&pen,20,20,80,60);});
            {
                Gdiplus::Bitmap mask(8,8,PixelFormat32bppARGB);
                const DWORD maskGdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
                oldCanvas.reset(); newCanvas.reset();
                for(int repeat=0;repeat<25;++repeat)
                    check(!PaintDrawingMask(newCanvas.dc(),newCanvas.dc(),{maximum-2,20,8,8},mask,false),
                          "Overflowing source extent must fail safely");
                pixelsCompared+=compareRgb(oldCanvas,newCanvas); ++rejectedCases;
                check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==maskGdiBefore,
                      "Rejected mask extent must not leak GDI resources");
            }
            check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==invalidGdiBefore,"Extreme input GDI cleanup");
            gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            results.push_back(measure("highlight_rectangle_small",newCanvas,600,
                [&]{reference::DrawHighlightedShape(DRAW_RECTANGLE,newCanvas.dc(),&brush,&pen,20,20,84,68);},
                [&]{DrawHighlightedShape(DRAW_RECTANGLE,newCanvas.dc(),&brush,&pen,20,20,84,68);}));
            results.push_back(measure("highlight_ellipse_large",newCanvas,80,
                [&]{reference::DrawHighlightedShape(DRAW_ELLIPSE,newCanvas.dc(),&brush,&pen,40,40,680,400);},
                [&]{DrawHighlightedShape(DRAW_ELLIPSE,newCanvas.dc(),&brush,&pen,40,40,680,400);}));
            results.push_back(measure("highlight_line_same_background",newCanvas,600,
                [&]{reference::DrawMaskedLine(newCanvas.dc(),newCanvas.dc(),{10,10,90,45},{17,17},{92,47},&pen,false);},
                [&]{DrawMaskedLine(newCanvas.dc(),newCanvas.dc(),{10,10,90,45},{17,17},{92,47},&pen,false);}));
            results.push_back(measure("highlight_line_saved_background",newCanvas,600,
                [&]{reference::DrawMaskedLine(newCanvas.dc(),background.dc(),{10,10,90,45},{17,17},{92,47},&pen,false);},
                [&]{DrawMaskedLine(newCanvas.dc(),background.dc(),{10,10,90,45},{17,17},{92,47},&pen,false);}));
            results.push_back(measure("blur_ellipse_small",newCanvas,250,
                [&]{reference::DrawBlurredShape(DRAW_ELLIPSE,&pen,newCanvas.dc(),nullptr,20,20,84,68);},
                [&]{DrawBlurredShape(DRAW_ELLIPSE,&pen,newCanvas.dc(),nullptr,20,20,84,68);}));
            gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            check(gdiBefore==gdiAfter,"Benchmarked effects must release GDI resources");
        }
        Gdiplus::GdiplusShutdown(token); token=0;
        std::cout<<"{\"passed\":true,\"rgb_comparison_cases\":"<<cases<<",\"rejected_input_cases\":"<<rejectedCases<<",\"pixels_compared\":"<<pixelsCompared
                 <<",\"median_trials\":9,\"gdi_before\":"<<gdiBefore<<",\"gdi_after\":"<<gdiAfter<<",\"benchmarks\":[";
        for(size_t i=0;i<results.size();++i) {
            if(i) std::cout<<',';
            const auto& r=results[i];
            std::cout<<"{\"name\":\""<<r.name<<"\",\"operations\":"<<r.operations<<",\"reference_ms\":"<<r.oldMs
                     <<",\"optimized_ms\":"<<r.newMs<<",\"speedup\":"<<r.oldMs/r.newMs<<'}';
        }
        std::cout<<"]}\n";
        return 0;
    } catch(const std::exception& error) {
        if(token) Gdiplus::GdiplusShutdown(token);
        std::cerr<<error.what()<<"\n";
        return 1;
    }
}
