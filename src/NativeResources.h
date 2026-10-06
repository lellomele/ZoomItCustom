#pragma once
#include <windows.h>
#include <utility>
namespace native {
template<class T, auto Destroy> class unique_resource {
    T value_{};
    static T Valid(T value) noexcept {
        return value && value != reinterpret_cast<T>(INVALID_HANDLE_VALUE) ? value : nullptr;
    }
public:
    unique_resource() noexcept = default;
    explicit unique_resource(T value) noexcept : value_(Valid(value)) {}
    ~unique_resource() { reset(); }
    unique_resource(const unique_resource&) = delete;
    unique_resource& operator=(const unique_resource&) = delete;
    unique_resource(unique_resource&& other) noexcept : value_(other.release()) {}
    unique_resource& operator=(unique_resource&& other) noexcept {
        if(this != &other) reset(other.release());
        return *this;
    }
    T get() const noexcept { return value_; }
    T release() noexcept { return std::exchange(value_, nullptr); }
    void reset(T value = nullptr) noexcept {
        value=Valid(value);
        if(value_==value)return;
        T previous=std::exchange(value_,value);
        if(previous) Destroy(previous);
    }
    explicit operator bool() const noexcept { return value_ != nullptr; }
    bool operator!=(std::nullptr_t) const noexcept { return value_ != nullptr; }
};
inline BOOL release_screen_dc(HDC dc) { return ReleaseDC(nullptr,dc); }
using unique_handle=unique_resource<HANDLE,CloseHandle>;
using unique_hwnd=unique_resource<HWND,DestroyWindow>;
using unique_hrgn=unique_resource<HRGN,DeleteObject>;
using unique_hbrush=unique_resource<HBRUSH,DeleteObject>;
using unique_hbitmap=unique_resource<HBITMAP,DeleteObject>;
using unique_hpen=unique_resource<HPEN,DeleteObject>;
using unique_screen_dc=unique_resource<HDC,release_screen_dc>;
using unique_memory_dc=unique_resource<HDC,DeleteDC>;

// Declare after the owning DC and object so restoration precedes their destruction.
class selected_object {
    HDC dc_{};
    HGDIOBJ previous_{};
public:
    selected_object(HDC dc,HGDIOBJ object) noexcept {
        if(!dc || !object)return;
        const HGDIOBJ previous=SelectObject(dc,object);
        if(previous && previous!=HGDI_ERROR){dc_=dc;previous_=previous;}
    }
    ~selected_object(){Restore();}
    selected_object(const selected_object&)=delete;
    selected_object& operator=(const selected_object&)=delete;
    selected_object(selected_object&& other) noexcept :
        dc_(std::exchange(other.dc_,nullptr)),previous_(std::exchange(other.previous_,nullptr)){}
    selected_object& operator=(selected_object&& other) noexcept {
        if(this!=&other){Restore();dc_=std::exchange(other.dc_,nullptr);previous_=std::exchange(other.previous_,nullptr);}
        return *this;
    }
    explicit operator bool() const noexcept{return dc_ && previous_;}
    HGDIOBJ previous() const noexcept{return previous_;}
    bool Restore() noexcept {
        if(!dc_ || !previous_)return true;
        const HGDIOBJ restored=SelectObject(dc_,previous_);
        dc_=nullptr;previous_=nullptr;
        return restored && restored!=HGDI_ERROR;
    }
};
}
