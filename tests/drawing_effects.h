#pragma once
#include <array>
#include <climits>
struct DrawingEffectsResults {
    size_t invalidCases{},captureFailures{},flushFailures{},pixelCases{},clippingCases{},alphaCases{},cycles{};
    DWORD gdiBefore{},gdiAfter{};
};
namespace drawing_test {
struct Graphics {
    Graphics(){require(g_GraphicsInit.Ready(),"Use the application's existing drawing graphics initialization");}
};
class Canvas {
    native::unique_memory_dc dc_{CreateCompatibleDC(nullptr)};
    native::unique_hbitmap image_;
    native::selected_object selected_{nullptr,nullptr};
    DWORD* pixels_{};
public:
    static constexpr int Width=64,Height=64;
    Canvas() {
        require(static_cast<bool>(dc_),"Create offscreen drawing fixture DC");
        BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),Width,-Height,1,32,BI_RGB};void* bits{};
        image_.reset(CreateDIBSection(dc_.get(),&info,DIB_RGB_COLORS,&bits,nullptr,0));
        require(image_ && bits,"Create offscreen drawing fixture bitmap");pixels_=static_cast<DWORD*>(bits);
        selected_=native::selected_object(dc_.get(),image_.get());require(static_cast<bool>(selected_),"Select offscreen drawing fixture bitmap");Reset();
    }
    HDC Dc()const noexcept{return dc_.get();}
    void Reset(){require(GdiFlush()!=FALSE,"Synchronize prior drawing before fixture reset");for(size_t i=0;i<Width*Height;++i)pixels_[i]=0xffffffffu;}
    std::array<DWORD,Width*Height> Snapshot()const {
        require(GdiFlush()!=FALSE,"Synchronize drawing pixels before comparison");std::array<DWORD,Width*Height> data{};
        memcpy(data.data(),pixels_,sizeof(data));return data;
    }
    void Gradient(bool opaque){Reset();for(int y=0;y<Height;++y)for(int x=0;x<Width;++x)
        pixels_[y*Width+x]=(opaque ? 0xff000000u : 0u) | (((x*3+y*2)%256)<<16) | (((x+y*4)%256)<<8) | ((x*5+y*3)%256);}
    void Checker(){Reset();for(int y=0;y<Height;++y)for(int x=0;x<Width;++x)pixels_[y*Width+x]=((x/4+y/4)%2) ? 0xffffffffu : 0xff000000u;}
};
inline bool YellowHighlight(COLORREF pixel) {return GetRValue(pixel)==255 && GetGValue(pixel)==255 && GetBValue(pixel)>=125 && GetBValue(pixel)<=128;}
}
size_t RunDrawingAlphaRegression(drawing_test::Canvas& canvas) {
    Gdiplus::Bitmap mask(64,64,PixelFormat32bppARGB);
    {Gdiplus::Graphics paint(&mask);require(paint.Clear(Gdiplus::Color(255,255,255,255))==Gdiplus::Ok,"Prepare identical opaque blur masks");paint.Flush(Gdiplus::FlushIntentionSync);}
    std::array<DWORD,64*64> opaqueRgb{};
    for(bool opaque:{true,false}) {
        canvas.Gradient(opaque);
        require(PaintDrawingMask(canvas.Dc(),canvas.Dc(),{0,0,64,64},mask,true),"Blur must accept both opaque and zero-alpha GDI source pixels");
        auto actual=canvas.Snapshot();for(auto& pixel:actual)pixel&=0x00ffffffu;
        if(opaque)opaqueRgb=actual;
        else {
            size_t mismatches{};for(size_t i=0;i<actual.size();++i)if(actual[i]!=opaqueRgb[i]) {
                if(mismatches<3)std::cerr<<"Blur alpha mismatch at "<<i<<": opaque="<<std::hex<<opaqueRgb[i]<<" zero="<<actual[i]<<std::dec<<"\n";
                ++mismatches;
            }
            require(!mismatches,"Undefined GDI alpha must not change the RGB result of the same desktop blur");
        }
    }
    return 2;
}

DrawingEffectsResults RunDrawingEffectsRegression() {
    DrawingEffectsResults result{};drawing_test::Graphics graphics;
    const DWORD oldColor=g_PenColor,oldWidth=g_PenWidth;const float oldBlur=g_BlurRadius;
    auto restore=zoomit::OnExit([&]{zoomit::runtime::Clear();g_PenColor=oldColor;g_PenWidth=oldWidth;g_BlurRadius=oldBlur;});
    g_PenColor=0x8000ffff;g_PenWidth=6;g_BlurRadius=4;
    drawing_test::Canvas canvas;Gdiplus::SolidBrush highlight(Gdiplus::Color(128,255,255,0));Gdiplus::Pen pen(Gdiplus::Color(255,0,0,0),6);
    Gdiplus::Bitmap mask(64,64,PixelFormat32bppARGB);
    {Gdiplus::Graphics brush(&mask);require(brush.Clear(Gdiplus::Color(255,255,255,255))==Gdiplus::Ok,"Build opaque effect mask");brush.Flush(Gdiplus::FlushIntentionSync);}
    const Gdiplus::Rect area(0,0,64,64);using zoomit::runtime::Api;
    // Complete the real first-use highlight and blur paths before accounting for retained resources.
    zoomit::runtime::Clear();canvas.Reset();
    require(PaintDrawingMask(canvas.Dc(),canvas.Dc(),area,mask,false),"Warm the actual highlight preparation and publication path");
    canvas.Checker();require(PaintDrawingMask(canvas.Dc(),canvas.Dc(),area,mask,true),"Warm the actual blur preparation and publication path");
    require(GdiFlush()!=FALSE,"Synchronize completed first-use drawing effects");canvas.Reset();zoomit::runtime::Clear();
    result.gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    const auto ownedBaseline=DrawingDib::Ownership();
    require(ownedBaseline==std::array<unsigned,3>{0,0,0},"Successful warm-up effects must release every native bitmap and DC without failed deletion");
    for(const Gdiplus::Rect invalid : {Gdiplus::Rect(-1,0,8,8),Gdiplus::Rect(0,0,65,64),Gdiplus::Rect(0,0,0,64),
            Gdiplus::Rect(INT_MAX-2,0,8,8),Gdiplus::Rect(0,0,32768,32768)}) {
        zoomit::runtime::Clear();DrawingDib denied(canvas.Dc(),invalid);
        require(!denied.pixels() && !zoomit::runtime::Calls(Api::Capture),"Invalid drawing areas must be rejected before capture or allocation");++result.invalidCases;
    }
    {Gdiplus::Bitmap smallMask(2,2,PixelFormat32bppARGB);const auto before=canvas.Snapshot();
        require(!PaintDrawingMask(canvas.Dc(),canvas.Dc(),area,smallMask,false) && canvas.Snapshot()==before,"Undersized masks must never read beyond their pixel rows");++result.invalidCases;}
    {DrawingBitmapLock lock(mask);require(lock.valid() && !lock.row(-1) && !lock.row(64),"Bitmap locks must reject row access outside their actual height");++result.invalidCases;}
    for(unsigned stage=1;stage<=4;++stage) {
        const auto ownership=DrawingDib::Ownership();zoomit::runtime::FailAt(Api::Capture,stage);
        {DrawingDib denied(canvas.Dc(),area);require(!denied.pixels(),"A failed effect capture must not expose incomplete DIB pixels");}
        require(zoomit::runtime::Calls(Api::Capture)==stage,"Each partial DIB capture failure must be reached");zoomit::runtime::Clear();
        require(DrawingDib::Ownership()==ownership,"Partial effect captures must successfully release every owned bitmap and DC");++result.captureFailures;
    }
    for(unsigned stage=1;stage<=9;++stage) {
        canvas.Reset();const auto before=canvas.Snapshot();const auto ownership=DrawingDib::Ownership();
        zoomit::runtime::FailAt(Api::Capture,stage);require(!PaintDrawingMask(canvas.Dc(),canvas.Dc(),area,mask,false),"All effect preparation and final-copy capture failures must be reported");
        require(zoomit::runtime::Calls(Api::Capture)==stage,"Numbered effect capture failure must hit its requested stage");zoomit::runtime::Clear();
        const auto after=canvas.Snapshot();const auto afterOwned=DrawingDib::Ownership();
        if(after!=before || afterOwned!=ownership) {
            size_t mismatches{},first=before.size();
            for(size_t pixel=0;pixel<before.size();++pixel)if(before[pixel]!=after[pixel]){if(first==before.size())first=pixel;++mismatches;}
            std::cerr<<"Effect failure invariant: stage="<<stage<<" owned_before="<<ownership[0]<<","<<ownership[1]<<","<<ownership[2]
                     <<" owned_after="<<afterOwned[0]<<","<<afterOwned[1]<<","<<afterOwned[2]<<" mismatched_pixels="<<mismatches;
            if(first<before.size())std::cerr<<" first_pixel="<<first<<" before="<<std::hex<<before[first]<<" after="<<after[first]<<std::dec;
            std::cerr<<"\n";
        }
        require(after==before,"Failed effect preparation must preserve every destination pixel");
        require(afterOwned==ownership,"Failed effect preparation must successfully release every owned bitmap and DC");++result.captureFailures;
    }
    for(unsigned stage=1;stage<=2;++stage) {
        canvas.Reset();const auto before=canvas.Snapshot();const auto ownership=DrawingDib::Ownership();zoomit::runtime::FailAt(Api::Flush,stage);
        require(!PaintDrawingMask(canvas.Dc(),canvas.Dc(),area,mask,false),"Failed GDI synchronization must reject effect pixel access");zoomit::runtime::Clear();
        require(canvas.Snapshot()==before,"Failed synchronization must preserve the destination image");
        require(DrawingDib::Ownership()==ownership,"Failed synchronization must successfully release every owned bitmap and DC");++result.flushFailures;
    }
    canvas.Reset();DrawHighlightedShape(DRAW_RECTANGLE,canvas.Dc(),&highlight,&pen,10,10,30,30);
    require(drawing_test::YellowHighlight(GetPixel(canvas.Dc(),20,20)) && GetPixel(canvas.Dc(),40,40)==RGB(255,255,255),"Normal highlights must blend only pixels inside their mask");++result.pixelCases;
    canvas.Reset();DrawHighlightedShape(DRAW_RECTANGLE,canvas.Dc(),&highlight,&pen,-20,-20,20,20);
    require(drawing_test::YellowHighlight(GetPixel(canvas.Dc(),5,5)) && GetPixel(canvas.Dc(),30,30)==RGB(255,255,255),"Clipped highlights must preserve the visible portion and untouched pixels");++result.clippingCases;
    canvas.Reset();DrawHighlightedShape(DRAW_ELLIPSE,canvas.Dc(),&highlight,&pen,-20,-20,40,40);
    require(drawing_test::YellowHighlight(GetPixel(canvas.Dc(),10,10)) && GetPixel(canvas.Dc(),60,60)==RGB(255,255,255),"Clipped ellipses must retain their original geometry");++result.clippingCases;
    canvas.Reset();DrawMaskedLine(canvas.Dc(),canvas.Dc(),{-20,29,120,6},{-20,32},{100,32},&pen,false);
    require(drawing_test::YellowHighlight(GetPixel(canvas.Dc(),0,32)) && drawing_test::YellowHighlight(GetPixel(canvas.Dc(),63,32)) && GetPixel(canvas.Dc(),30,10)==RGB(255,255,255),"Clipped line masks must cover their visible line without affecting other rows");++result.clippingCases;
    canvas.Reset();DrawHighlightedShape(DRAW_RECTANGLE,canvas.Dc(),&highlight,&pen,INT_MIN,INT_MIN,INT_MAX,INT_MAX);
    require(drawing_test::YellowHighlight(GetPixel(canvas.Dc(),32,32)),"Extreme coordinates must clip to the actual small canvas before mask allocation");++result.clippingCases;
    canvas.Reset();const auto unchanged=canvas.Snapshot();zoomit::runtime::Clear();
    DrawHighlightedShape(DRAW_RECTANGLE,canvas.Dc(),&highlight,&pen,INT_MIN,INT_MIN,-100,-100);
    require(canvas.Snapshot()==unchanged && !zoomit::runtime::Calls(Api::Capture),"Wholly offscreen effects must return before allocating or capturing");++result.clippingCases;
    canvas.Checker();const COLORREF sharp=GetPixel(canvas.Dc(),20,20);DrawBlurredShape(DRAW_RECTANGLE,&pen,canvas.Dc(),nullptr,10,10,40,40);
    const COLORREF softened=GetPixel(canvas.Dc(),20,20);
    require(softened!=sharp && GetRValue(softened)>0 && GetRValue(softened)<255,"Blur must still soften opaque source pixels after buffer validation");++result.pixelCases;
    result.alphaCases=RunDrawingAlphaRegression(canvas);
    for(unsigned i=0;i<30;++i){canvas.Reset();DrawHighlightedShape(DRAW_RECTANGLE,canvas.Dc(),&highlight,&pen,-2,-2,30,30);DrawBlurredShape(DRAW_ELLIPSE,&pen,canvas.Dc(),nullptr,-2,-2,30,30);
        require(DrawingDib::Ownership()==ownedBaseline,"Every completed highlight and blur cycle must release its owned native resources");++result.cycles;}
    require(DrawingDib::Ownership()==ownedBaseline,"All drawing effects and failure paths must release their native resources without failed deletion");
    // GDI+ owns a notification thread that may create and release unrelated startup objects.
    // Verify our objects exactly above, then bound whole-process retained growth after it settles.
    const ULONGLONG deadline=GetTickCount64()+2500;
    result.gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    while(result.gdiAfter>result.gdiBefore && GetTickCount64()<deadline) {
        require(GdiFlush()!=FALSE,"Synchronize completed effects while checking retained process resources");
        Sleep(1);result.gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    }
    require(result.gdiAfter>=2 && result.gdiAfter<=result.gdiBefore,"Completed drawing effects must not retain additional process GDI resources");return result;
}
void PrintDrawingEffectsResults(const DrawingEffectsResults& r) {
    std::cout<<"{\"passed\":true,\"drawing_effects\":true,\"invalid_inputs\":"<<r.invalidCases<<",\"capture_failure_cases\":"<<r.captureFailures
        <<",\"flush_failure_cases\":"<<r.flushFailures<<",\"pixel_cases\":"<<r.pixelCases<<",\"clipping_cases\":"<<r.clippingCases
        <<",\"gdi_alpha_cases\":"<<r.alphaCases<<",\"cycles\":"<<r.cycles<<",\"gdi_before\":"<<r.gdiBefore<<",\"gdi_after\":"<<r.gdiAfter<<",\"owned_dcs\":"<<DrawingDib::liveDcs.load()<<",\"owned_bitmaps\":"<<DrawingDib::liveBitmaps.load()
        <<",\"native_delete_failures\":"<<DrawingDib::deleteFailures.load()<<",\"visible_windows\":0}\n";
}
