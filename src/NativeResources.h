#pragma once
namespace native {
template<class T, auto Destroy> class unique_resource {
    T value_{};
public:
    unique_resource() noexcept = default;
    explicit unique_resource(T value) noexcept : value_(value) {}
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
        T previous=std::exchange(value_,value);
        if(previous) Destroy(previous);
    }
    bool operator!=(std::nullptr_t) const noexcept { return value_ != nullptr; }
};
inline BOOL release_screen_dc(HDC dc) { return ReleaseDC(nullptr,dc); }
using unique_handle=unique_resource<HANDLE,CloseHandle>;
using unique_hwnd=unique_resource<HWND,DestroyWindow>;
using unique_hrgn=unique_resource<HRGN,DeleteObject>;
using unique_hbrush=unique_resource<HBRUSH,DeleteObject>;
using unique_screen_dc=unique_resource<HDC,release_screen_dc>;
}

