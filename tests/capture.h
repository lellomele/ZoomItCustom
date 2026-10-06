#pragma once
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

struct CaptureResults {
    size_t captureFailures{},flushFailures{},encoderFailures{},invalidInputs{},successCases{},cycles{};
    DWORD gdiBefore{},gdiAfter{};
};

namespace capture_test {
class Source {
    native::unique_memory_dc dc_{CreateCompatibleDC(nullptr)};
    native::unique_hbitmap bitmap_;
    native::selected_object selected_{nullptr,nullptr};
public:
    static constexpr int Width=8,Height=6;
    static COLORREF ColorAt(int x,int y) noexcept {
        return RGB(24+x*19,31+y*27,18+x*11+y*9);
    }
    Source() {
        require(static_cast<bool>(dc_),"Create an offscreen source DC");
        BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),Width,-Height,1,32,BI_RGB};
        void* bits{};bitmap_.reset(CreateDIBSection(dc_.get(),&info,DIB_RGB_COLORS,&bits,nullptr,0));
        require(bitmap_ && bits,"Allocate the isolated source pixels");
        selected_=native::selected_object(dc_.get(),bitmap_.get());
        require(static_cast<bool>(selected_),"Select the source bitmap into its owned DC");
        auto pixels=static_cast<std::uint32_t*>(bits);
        for(int y=0;y<Height;++y)for(int x=0;x<Width;++x) {
            const COLORREF color=ColorAt(x,y);
            pixels[y*Width+x]=0xff000000u | (GetRValue(color)<<16) | (GetGValue(color)<<8) | GetBValue(color);
        }
        require(GdiFlush()!=FALSE,"Synchronize the offscreen source before capture");
    }
    HDC Dc() const noexcept{return dc_.get();}
};

class Files {
    std::wstring folder_;
public:
    std::wstring target;
    Files() {
        wchar_t temporary[MAX_PATH]{},identity[40]{};GUID id{};
        const DWORD temporaryLength=GetTempPathW(MAX_PATH,temporary);
        require(temporaryLength>0 && temporaryLength<MAX_PATH && CoCreateGuid(&id)==S_OK &&
                StringFromGUID2(id,identity,_countof(identity))>0,"Choose an isolated capture fixture directory");
        folder_=temporary;folder_+=L"ZoomItCustom-capture-";folder_+=identity;
        require(CreateDirectoryW(folder_.c_str(),nullptr)!=FALSE,"Create a fresh capture fixture directory");
        target=folder_+L"\\image.png";
    }
    Files(const Files&)=delete;
    ~Files() {
        WIN32_FIND_DATAW data{};const auto pattern=folder_+L"\\*";
        HANDLE find=FindFirstFileW(pattern.c_str(),&data);
        if(find!=INVALID_HANDLE_VALUE) {
            do {if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) {
                const auto file=folder_+L"\\"+data.cFileName;DeleteFileW(file.c_str());
            }}while(FindNextFileW(find,&data));
            FindClose(find);
        }
        RemoveDirectoryW(folder_.c_str());
    }
    std::wstring MissingTarget() const{return folder_+L"\\missing\\image.png";}
    size_t Count() const {
        WIN32_FIND_DATAW data{};const auto pattern=folder_+L"\\*";
        HANDLE find=FindFirstFileW(pattern.c_str(),&data);
        require(find!=INVALID_HANDLE_VALUE,"Enumerate only the isolated capture fixture directory");
        const auto close=zoomit::OnExit([&]{FindClose(find);});
        size_t count=0;
        do {if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))++count;}while(FindNextFileW(find,&data));
        require(GetLastError()==ERROR_NO_MORE_FILES,"Complete isolated file enumeration");
        return count;
    }
    void Write(const std::vector<BYTE>& bytes) const {
        native::unique_handle file(CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
        require(static_cast<bool>(file),"Write the existing destination fixture");DWORD written{};
        require(WriteFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)!=FALSE && written==bytes.size(),
                "Write the complete existing destination fixture");
    }
    std::vector<BYTE> Read() const {
        native::unique_handle file(CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                               nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        require(static_cast<bool>(file),"Read the existing destination fixture");
        LARGE_INTEGER size{};require(GetFileSizeEx(file.get(),&size)!=FALSE && size.QuadPart>=0 && size.QuadPart<1024*1024,
                                      "Bound the isolated PNG fixture read");
        std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));DWORD read{};
        require(ReadFile(file.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)!=FALSE && read==bytes.size(),
                "Read the complete existing destination fixture");
        return bytes;
    }
};

inline void VerifyPixels(HBITMAP handle) {
    Gdiplus::Bitmap image(handle,nullptr);
    require(image.GetLastStatus()==Gdiplus::Ok && image.GetWidth()==3 && image.GetHeight()==2,"Read captured crop dimensions");
    for(int y=0;y<2;++y)for(int x=0;x<3;++x) {
        Gdiplus::Color color;
        require(image.GetPixel(x,y,&color)==Gdiplus::Ok,"Read real captured crop pixels");
        const auto expected=Source::ColorAt(x+1,y+2);
        require(color.GetR()==GetRValue(expected) && color.GetG()==GetGValue(expected) && color.GetB()==GetBValue(expected),
                "A captured crop must contain the selected source region");
    }
}
inline void VerifyPng(const Files& files) {
    const auto bytes=files.Read();constexpr std::array<BYTE,8> signature{137,80,78,71,13,10,26,10};
    require(bytes.size()>signature.size() && std::equal(signature.begin(),signature.end(),bytes.begin()),
            "Successful save must create a complete PNG signature");
    Gdiplus::Bitmap image(files.target.c_str());
    require(image.GetLastStatus()==Gdiplus::Ok && image.GetWidth()==3 && image.GetHeight()==2,"Decode the saved PNG with its crop dimensions");
    for(int y=0;y<2;++y)for(int x=0;x<3;++x) {
        Gdiplus::Color color;require(image.GetPixel(x,y,&color)==Gdiplus::Ok,"Decode saved PNG pixels");
        const auto expected=Source::ColorAt(x+1,y+2);
        require(color.GetR()==GetRValue(expected) && color.GetG()==GetGValue(expected) && color.GetB()==GetBValue(expected),
                "PNG encoding must preserve the captured source pixels");
    }
}
} // namespace capture_test

CaptureResults RunCaptureRegression() {
    using zoomit::runtime::Api;
    static_assert(!std::is_copy_constructible_v<CapturedBitmap>);
    static_assert(!std::is_copy_assignable_v<CapturedBitmap>);
    CaptureResults result{};zoomit::runtime::Clear();
    const auto clear=zoomit::OnExit([]{zoomit::runtime::Clear();});
    require(g_GraphicsInit.Ready(),"Use the application's existing capture codec initialization");
    capture_test::Source source;capture_test::Files files;
    const auto captureCrop=[&](CapturedBitmap& image) {return image.Capture(source.Dc(),1,2,3,2,3,2);};
    // Warm native capture/codec resources before resource accounting.
    {
        CapturedBitmap image;require(captureCrop(image),"Capture a real offscreen crop");
        const HBITMAP bitmap=image.ReadyImage();require(bitmap && !image.Dc(),"Detach a captured bitmap from its DC before encoding");
        capture_test::VerifyPixels(bitmap);
        require(SavePng(files.target.data(),bitmap)==ERROR_SUCCESS,"Save the initial isolated PNG fixture");
        capture_test::VerifyPng(files);require(files.Count()==1,"Successful encoding must remove its temporary file");
    }
    result.gdiBefore=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(unsigned step=1;step<=4;++step) {
        zoomit::runtime::FailAt(Api::Capture,step);
        {CapturedBitmap image;require(!captureCrop(image),"Each native capture stage must report its own injected failure");}
        require(zoomit::runtime::Calls(Api::Capture)==step,"Capture must stop at the failed stage");
        zoomit::runtime::Clear();
        require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==result.gdiBefore,"Partial captures must release every acquired GDI resource");
        ++result.captureFailures;
    }
    zoomit::runtime::FailAt(Api::Flush,1);
    {CapturedBitmap image;require(!captureCrop(image),"An unsuccessful GDI flush must reject the capture");}
    require(zoomit::runtime::Calls(Api::Capture)==4 && zoomit::runtime::Calls(Api::Flush)==1,"Flush failure must occur after the completed capture stages");
    zoomit::runtime::Clear();
    require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==result.gdiBefore,"A failed flush must release the selected capture bitmap and DC");
    ++result.flushFailures;
    for(const auto dimensions:std::array<std::array<int,4>,7>{{{{0,2,3,2}},{{3,0,3,2}},{{3,2,0,2}},{{3,2,3,-1}},{{3,2,32769,2}},{{3,2,2,32769}},{{3,2,20000,20000}}}}) {
        CapturedBitmap image;
        require(!image.Capture(source.Dc(),0,0,dimensions[0],dimensions[1],dimensions[2],dimensions[3]),
                "Invalid capture dimensions must be rejected before resource acquisition");
        require(!image.Image() && !image.Dc() && zoomit::runtime::Calls(Api::Capture)==0,"Invalid capture input must acquire no resources");
        ++result.invalidInputs;
    }
    {CapturedBitmap image;require(!image.Capture(nullptr,0,0,3,2,3,2),"A null source DC must fail safely");++result.invalidInputs;}
    const std::vector<BYTE> sentinel{'e','x','i','s','t','i','n','g',0,1,2,3,255};
    for(unsigned step=1;step<=3;++step) {
        files.Write(sentinel);zoomit::runtime::Clear();
        {
            CapturedBitmap image;require(captureCrop(image),"Prepare a detached crop for encoder failure");
            zoomit::runtime::FailAt(Api::Encoder,step);
            require(SavePng(files.target.data(),image.ReadyImage())!=ERROR_SUCCESS,"Each encoder stage must return an explicit failure");
            require(zoomit::runtime::Calls(Api::Encoder)==step,"Encoder failure must stop at the requested stage");
        }
        zoomit::runtime::Clear();
        require(files.Read()==sentinel && files.Count()==1,"Encoder failure must preserve the previous file byte for byte and remove temporary files");
        require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==result.gdiBefore,"Encoder failure must not retain captured GDI resources");
        ++result.encoderFailures;
    }
    {
        CapturedBitmap image;require(captureCrop(image),"Prepare image validation cases");
        const auto bitmap=image.ReadyImage();
        require(SavePng(nullptr,bitmap)==ERROR_INVALID_PARAMETER,"A null output filename must return a validation error");
        wchar_t empty[]{L'\0'};require(SavePng(empty,bitmap)==ERROR_INVALID_PARAMETER,"An empty output filename must return a validation error");
        require(SavePng(files.target.data(),nullptr)==ERROR_NOT_ENOUGH_MEMORY,"A null bitmap must return an error without changing the destination");
        auto missing=files.MissingTarget();require(SavePng(missing.data(),bitmap)!=ERROR_SUCCESS,"A missing parent directory must return an error");
        std::wstring excessive(MAX_PATH+40,L'x');require(SavePng(excessive.data(),bitmap)==ERROR_FILENAME_EXCED_RANGE,"An unsupported path length must be rejected cleanly");
        require(files.Read()==sentinel && files.Count()==1,"Invalid destinations must preserve existing files and leave no temporary files");
        result.invalidInputs+=5;
    }
    {
        CapturedBitmap image;require(captureCrop(image),"Prepare a crop for atomic replacement failure");
        native::unique_handle locked(CreateFileW(files.target.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
                                                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        require(static_cast<bool>(locked),"Lock only the isolated destination against replacement");
        require(SavePng(files.target.data(),image.ReadyImage())!=ERROR_SUCCESS,"A destination that denies replacement must return an error");
        require(files.Read()==sentinel && files.Count()==1,"Failed final replacement must retain the original file and delete the completed temporary PNG");
        ++result.encoderFailures;
    }
    for(int cycle=0;cycle<20;++cycle) {
        zoomit::runtime::Clear();
        {
            CapturedBitmap image;require(captureCrop(image),"Repeated offscreen capture must succeed");
            const auto bitmap=image.ReadyImage();require(bitmap==image.ReadyImage() && !image.Dc(),"ReadyImage must be idempotent without losing bitmap ownership");
            require(SavePng(files.target.data(),bitmap)==ERROR_SUCCESS,"Replace the isolated destination with a completed PNG");
            capture_test::VerifyPng(files);require(files.Count()==1,"Repeated saves must remove their intermediate files");
        }
        require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==result.gdiBefore,"Repeated capture and encoding must not accumulate GDI objects");
        ++result.cycles;
    }
    {
        CapturedBitmap image;require(captureCrop(image),"Prepare a reusable offscreen capture owner");
        const DWORD active=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        for(int cycle=0;cycle<12;++cycle) {
            require(captureCrop(image),"A capture owner must safely replace its previous bitmap and DC");
            require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==active,"Reusing a capture owner must not leak its previous resources");
        }
        capture_test::VerifyPixels(image.ReadyImage());++result.successCases;
    }
    {
        CapturedBitmap image;zoomit::runtime::FailAt(Api::Capture,4);
        require(!captureCrop(image),"Prepare a reusable owner with a selected partial capture");
        zoomit::runtime::Clear();require(captureCrop(image),"A capture owner must recover safely from its previous partial preparation");
        capture_test::VerifyPixels(image.ReadyImage());++result.successCases;
    }
    {
        CapturedBitmap image;require(image.Capture(source.Dc(),0,0,8,6,16,12),"Resized offscreen capture must succeed");
        Gdiplus::Bitmap scaled(image.ReadyImage(),nullptr);
        require(scaled.GetLastStatus()==Gdiplus::Ok && scaled.GetWidth()==16 && scaled.GetHeight()==12,"Resized captures must retain the requested dimensions");
        ++result.successCases;
    }
    {
        CapturedBitmap image;require(captureCrop(image),"Prepare a capture ownership transfer");
        native::unique_hbitmap bitmap(image.ReleaseImage());
        require(bitmap && !image.Image() && !image.Dc(),"Ownership transfer must detach the bitmap and DC exactly once");
        capture_test::VerifyPixels(bitmap.get());++result.successCases;
    }
    result.successCases+=2;
    result.gdiAfter=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    require(result.gdiAfter==result.gdiBefore,"Capture validation, failure, encoding and ownership transfer must leave no GDI resources behind");
    return result;
}
void PrintCaptureResults(const CaptureResults& result) {
    std::cout<<"{\"passed\":true,\"capture\":true,\"capture_failure_stages\":"<<result.captureFailures
        <<",\"flush_failure_cases\":"<<result.flushFailures<<",\"encoder_and_replacement_failure_cases\":"<<result.encoderFailures
        <<",\"invalid_input_cases\":"<<result.invalidInputs<<",\"success_cases\":"<<result.successCases
        <<",\"capture_encode_cycles\":"<<result.cycles<<",\"gdi_before\":"<<result.gdiBefore<<",\"gdi_after\":"<<result.gdiAfter<<"}\n";
}
