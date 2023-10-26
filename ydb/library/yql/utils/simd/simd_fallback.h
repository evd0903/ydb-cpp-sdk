#pragma once

#include <cstdint>
#include <immintrin.h>

#include <util/system/types.h>
#include <util/stream/output.h>
#include <util/generic/string.h>

namespace NSimd {
namespace NFallback {

template <typename T>
struct TSimd8;

template<typename Child>
struct TBase {
    ui64 Value;

    inline TBase()
        : Value{ui64()} {
    }

    inline TBase(const ui64 value)
        : Value(value) {
    }

    explicit inline operator const ui64&() const {
        return this->Value;
    }
    explicit inline operator ui64&() {
        return this->Value;
    }

    inline Child operator|(const Child other) const {
        return this->Value | other.Value;
    }
    inline Child operator&(const Child other) const {
        return this->Value & other.Value;
    }
    inline Child operator^(const Child other) const {
        return this->Value ^ other.Value;
    }
    inline Child BitAndNot(const Child other) const {
        return (~this->Value) & other.Value;
    };
    inline Child& operator|=(const Child other) {
        auto cast = static_cast<Child*>(*this);
        *cast = *cast | other;
        return *cast;
    }
    inline Child& operator&=(const Child other) {
        auto cast = static_cast<Child*>(*this);
        *cast = *cast & other;
        return *cast;
    };
    inline Child& operator^=(const Child other) {
        auto cast = static_cast<Child*>(*this);
        *cast = *cast ^ other;
        return *cast;
    };
};

template<typename T, typename Mask=TSimd8<bool>>
struct TBase8: TBase<TSimd8<T>> {

    inline TBase8()
        : TBase<TSimd8<T>>()
    {
    }
    
    inline TBase8(const ui64 _value)
        : TBase<TSimd8<T>>(_value)
    {
    }

    template<int N>
    inline TSimd8<T> Blend16(const TSimd8<T> other) {
        ui64 dst = 0;
        size_t j = (1 << 16) - 1;
        for (size_t i = 0; i < 4; i += 1, j <<= 16) {
            if (N & (1LL << i)) {
                dst |= other->Value & j;
            } else {
                dst |= this->Value & j;
            }
        }
        return TSimd8<T>(dst);
    }

    inline TSimd8<T> BlendVar(const TSimd8<T> other, const TSimd8<T> mask) {
        ui64 dst = 0;
        size_t j = (1 << 8) - 1;
        for (size_t i = 0; i < 8; i += 1, j <<= 8) {
            if (mask.Value & (1LL << i)) {
                dst |= other->Value & j;
            } else {
                dst |= this->Value & j;
            }
        }
        return TSimd8<T>(dst);
    }

    friend inline Mask operator==(const TSimd8<T> lhs, const TSimd8<T> rhs) {
        return lhs.Value == rhs.Value;
    }

    static const int SIZE = sizeof(TBase<T>::Value);
};

template<>
struct TSimd8<bool>: TBase8<bool> {

    inline TSimd8<bool>()
        : TBase8()
    {
    }
    
    inline TSimd8<bool>(const ui64 value)
        : TBase8<bool>(value)
    {
    }
    
    inline TSimd8<bool>(bool value)
        : TBase8<bool>(Set(value))
    {
    }

    static inline TSimd8<bool> Set(bool value) {
        return ui64(-value);
    }

    inline bool Any() const {
        return Value != 0;
    }
    
    inline TSimd8<bool> operator~() const {
        return *this ^ true;
    }
};

template<typename T>
struct TBase8Numeric: TBase8<T> {
   
    inline TBase8Numeric()
        : TBase8<T>()
    {
    }
    inline TBase8Numeric(const ui64 value)
        : TBase8<T>(value)
    {
    }

    static inline TSimd8<T> Set(T value) {
        TSimd8<T> result = TSimd8<T>::Zero();
        auto dst = (ui8*)(&result.Value);
        dst[0] = dst[1] = dst[2] = dst[3] = value;
        dst[4] = dst[5] = dst[6] = dst[7] = value;
        return result;
    }
    static inline TSimd8<T> Zero() {
        return (ui64) 0;
    }
    static inline TSimd8<T> Load(const T values[8]) {
        return TSimd8<T>(*((const ui64*) values));
    }

    static inline TSimd8<T> LoadAligned(const T values[8]) {
        return Load(values);
    }

    static inline TSimd8<T> LoadStream(const T values[8]) {
        return Load(values);
    }

    inline void Store(T dst[8]) const {
        *((ui64*) dst) = this->Value;
    }

    inline void StoreAligned(T dst[8]) const {
        Store(dst);
    }

    inline void StoreStream(T dst[8]) const {
        Store(dst);
    }

    template<typename TOut>
    void Log(IOutputStream& out, TString delimeter = " ", TString end = "\n") {
        const size_t n = sizeof(this->Value) / sizeof(TOut);
        TOut buf[n];
        Store((i8*) buf);
        if (n == sizeof(this->Value)) {
            for (size_t i = 0; i < n; i += 1) {
                out << int(buf[i]);
                if (i + 1 < n) {
                    out << delimeter;
                } else {
                    out << end;
                }
            }
        } else {
            for (size_t i = 0; i < n; i += 1) {
                out << buf[i];
                if (i + 1 < n) {
                    out << delimeter;
                } else {
                    out << end;
                }
            }
        }
    }

    inline TSimd8<T> operator+(const TSimd8<T> other) const {
        return this->Value + other.Value;
    }
    inline TSimd8<T> operator-(const TSimd8<T> other) const {
        return this->Value - other.Value;
    }
    inline TSimd8<T>& operator+=(const TSimd8<T> other) {
        *this = *this + other;
        return *static_cast<TSimd8<T>*>(this);
    }
    inline TSimd8<T>& operator-=(const TSimd8<T> other) {
        *this = *this - other;
        return *static_cast<TSimd8<T>*>(this);
    }

    // 0xFFu = 11111111 = 2^8 - 1
    inline TSimd8<T> operator~() const {
        return *this ^ ui8(0xFFu);
    }
};

template<>
struct TSimd8<i8> : TBase8Numeric<i8> {
    inline TSimd8()
        : TBase8Numeric<i8>()
    {    
    }    
    inline TSimd8(const ui64 value)
        : TBase8Numeric<i8>(value)
    {
    }
    inline TSimd8(i8 value)
        : TSimd8(Set(value))
    {
    }
    inline TSimd8(const i8 values[8])
        : TSimd8(Load(values))
    {
    }
    inline TSimd8(
        i8 v0,  i8 v1,  i8 v2,  i8 v3,  i8 v4,  i8 v5,  i8 v6,  i8 v7
    ) : TSimd8({v0, v1, v2, v3, v4, v5, v6, v7})
    {
    }

    inline TSimd8<i8> MaxValue(const TSimd8<i8> other) const {
        return (*this > other).Any() ? *this : other;
    }
    inline TSimd8<i8> MinValue(const TSimd8<i8> other) const {
        return (*this < other).Any() ? *this : other;
    }

    inline TSimd8<bool> operator>(const TSimd8<i8> other) const {
        return i64(this->Value) > i64(other.Value);
    }
    inline TSimd8<bool> operator<(const TSimd8<i8> other) const {
        return i64(this->Value) < i64(other.Value);
    }
};

template<>
struct TSimd8<ui8>: TBase8Numeric<ui8> {
    inline TSimd8()
        : TBase8Numeric<ui8>()
    {
    }
    inline TSimd8(const ui64 _value)
        : TBase8Numeric<ui8>(_value) 
    {
    }
    inline TSimd8(ui8 _value)
        : TSimd8(Set(_value))
    {
    }
    inline TSimd8(const ui8 values[8])
        : TSimd8(Load(values)) 
    {
    }
    inline TSimd8(
        ui8 v0,  ui8 v1,  ui8 v2,  ui8 v3,  ui8 v4,  ui8 v5,  ui8 v6,  ui8 v7
    ) : TSimd8({v0, v1, v2, v3, v4, v5, v6, v7}
    ) {}

    inline TSimd8<ui8> MaxValue(const TSimd8<ui8> other) const {
        return this->Value > other.Value ? *this : other;
    }
    inline TSimd8<ui8> MinValue(const TSimd8<ui8> other) const {
        return this->Value < other.Value ? *this : other;
    }

    inline TSimd8<bool> operator<=(const TSimd8<ui8> other) const {
        return other.MaxValue(*this) == other;
    }
    inline TSimd8<bool> operator>=(const TSimd8<ui8> other) const {
        return other.MinValue(*this) == other;
    }

    inline TSimd8<bool> BitsNotSet() const {
        return this->Value == 0;
    }
    inline TSimd8<bool> AnyBitsSet() const {
        return ~this->BitsNotSet();
    }
    inline bool BitsNotSetAnywhere() const {
        return BitsNotSet().Any();
    }
    inline bool AnyBitsSetAnywhere() const {
        return !BitsNotSetAnywhere();
    }
    inline bool BitsNotSetAnywhere(TSimd8<ui8> bits) const {
        return ((*this) & bits).Value == 0;
    }
    inline bool AnyBitsSetAnywhere(TSimd8<ui8> bits) const {
        return !BitsNotSetAnywhere(bits);
    }
};

}
}
