namespace {{ ci.namespace()|detail_cpp }} {

/// A helper class to read values out of a Rust byte buffer.
class MOZ_STACK_CLASS Reader final {
 public:
  explicit Reader(const RustBuffer& aBuffer) : mBuffer(aBuffer), mOffset(0) {}

  /// Indicates if the offset has reached the end of the buffer.
  bool HasRemaining() {
    return static_cast<int64_t>(mOffset.value()) < mBuffer.mLen;
  }

  /// Helpers to read fixed-width primitive types at the current offset.
  /// Fixed-width integers are read in big endian order.

  uint8_t ReadUInt8() {
    return ReadAt<uint8_t>(
        [this](size_t aOffset) { return mBuffer.mData[aOffset]; });
  }

  int8_t ReadInt8() { return BitwiseCast<int8_t>(ReadUInt8()); }

  uint16_t ReadUInt16() {
    return ReadAt<uint16_t>([this](size_t aOffset) {
      uint16_t value;
      memcpy(&value, &mBuffer.mData[aOffset], sizeof(uint16_t));
      return PR_ntohs(value);
    });
  }

  int16_t ReadInt16() { return BitwiseCast<int16_t>(ReadUInt16()); }

  uint32_t ReadUInt32() {
    return ReadAt<uint32_t>([this](size_t aOffset) {
      uint32_t value;
      memcpy(&value, &mBuffer.mData[aOffset], sizeof(uint32_t));
      return PR_ntohl(value);
    });
  }

  int32_t ReadInt32() { return BitwiseCast<int32_t>(ReadUInt32()); }

  uint64_t ReadUInt64() {
    return ReadAt<uint64_t>([this](size_t aOffset) {
      uint64_t value;
      memcpy(&value, &mBuffer.mData[aOffset], sizeof(uint64_t));
      return PR_ntohll(value);
    });
  }

  int64_t ReadInt64() { return BitwiseCast<int64_t>(ReadUInt64()); }

  float ReadFloat() { return BitwiseCast<float>(ReadUInt32()); }

  double ReadDouble() { return BitwiseCast<double>(ReadUInt64()); }

  /// Reads a length-prefixed UTF-8 encoded string at the current offset. The
  /// closure takes a `Span` pointing to the raw bytes, which it can use to
  /// copy the bytes into an `nsCString` or `nsString`.
  ///
  /// Safety: The closure must copy the span's contents into a new owned string.
  /// It must not hold on to the span, as its contents will be invalidated when
  /// the backing Rust byte buffer is freed. It must not call any other methods
  /// on the reader.
  template <typename T>
  void ReadRawString(
      T& aString,
      const std::function<void(Span<const char>, T& aString)>& aClosure) {
    uint32_t length = ReadUInt32();
    CheckedInt<size_t> newOffset = mOffset;
    newOffset += length;
    AssertInBounds(newOffset);
    const char* begin =
        reinterpret_cast<const char*>(&mBuffer.mData[mOffset.value()]);
    aClosure(Span(begin, length), aString);
    mOffset = newOffset;
  }

 private:
  void AssertInBounds(const CheckedInt<size_t>& aNewOffset) const {
    MOZ_RELEASE_ASSERT(aNewOffset.isValid() &&
                       static_cast<int64_t>(aNewOffset.value()) <=
                           mBuffer.mLen);
  }

  template <typename T>
  T ReadAt(const std::function<T(size_t)>& aClosure) {
    CheckedInt<size_t> newOffset = mOffset;
    newOffset += sizeof(T);
    AssertInBounds(newOffset);
    T result = aClosure(mOffset.value());
    mOffset = newOffset;
    return result;
  }

  const RustBuffer& mBuffer;
  CheckedInt<size_t> mOffset;
};

class MOZ_STACK_CLASS Writer final {
 public:
  explicit Writer(size_t aCapacity) : mBuffer(aCapacity) {}

  void WriteUInt8(const uint8_t& aValue) {
    WriteAt<uint8_t>(aValue, [this](size_t aOffset, const uint8_t& aValue) {
      mBuffer[aOffset] = aValue;
    });
  }

  void WriteInt8(const int8_t& aValue) {
    WriteUInt8(BitwiseCast<uint8_t>(aValue));
  }

  // This code uses `memcpy` and other eye-twitchy patterns because it
  // originally wrote values directly into a `RustBuffer`, instead of
  // an intermediate `nsTArray`. Once #251 is fixed, we can return to
  // doing that, and remove `ToRustBuffer`.

  void WriteUInt16(const uint16_t& aValue) {
    WriteAt<uint16_t>(aValue, [this](size_t aOffset, const uint16_t& aValue) {
      uint16_t value = PR_htons(aValue);
      memcpy(&mBuffer.Elements()[aOffset], &value, sizeof(uint16_t));
    });
  }

  void WriteInt16(const int16_t& aValue) {
    WriteUInt16(BitwiseCast<uint16_t>(aValue));
  }

  void WriteUInt32(const uint32_t& aValue) {
    WriteAt<uint32_t>(aValue, [this](size_t aOffset, const uint32_t& aValue) {
      uint32_t value = PR_htonl(aValue);
      memcpy(&mBuffer.Elements()[aOffset], &value, sizeof(uint32_t));
    });
  }

  void WriteInt32(const int32_t& aValue) {
    WriteUInt32(BitwiseCast<uint32_t>(aValue));
  }

  void WriteUInt64(const uint64_t& aValue) {
    WriteAt<uint64_t>(aValue, [this](size_t aOffset, const uint64_t& aValue) {
      uint64_t value = PR_htonll(aValue);
      memcpy(&mBuffer.Elements()[aOffset], &value, sizeof(uint64_t));
    });
  }

  void WriteInt64(const int64_t& aValue) {
    WriteUInt64(BitwiseCast<uint64_t>(aValue));
  }

  void WriteFloat(const float& aValue) {
    WriteUInt32(BitwiseCast<uint32_t>(aValue));
  }

  void WriteDouble(const double& aValue) {
    WriteUInt64(BitwiseCast<uint64_t>(aValue));
  }

  /// Writes a length-prefixed UTF-8 encoded string at the current offset. The
  /// closure takes a `Span` pointing to the byte buffer, which it should fill
  /// with bytes and return the actual number of bytes written.
  ///
  /// This function is (more than a little) convoluted. It's written this way
  /// because we want to support UTF-8 and UTF-16 strings. The "size hint" is
  /// the maximum number of bytes that the closure can write. For UTF-8 strings,
  /// this is just the length. For UTF-16 strings, which must be converted to
  /// UTF-8, this can be up to three times the length. Once the closure tells us
  /// how many bytes it's actually written, we can write the length prefix, and
  /// advance the current offset.
  ///
  /// Safety: The closure must copy the string's contents into the span, and
  /// return the exact number of bytes it copied. Returning the wrong count can
  /// either truncate the string, or leave uninitialized memory in the buffer.
  /// The closure must not call any other methods on the writer.
  void WriteRawString(size_t aSizeHint,
                      const std::function<size_t(Span<char>)>& aClosure) {
    // First, make sure the buffer is big enough to hold the length prefix.
    // We'll start writing our string directly after the prefix.
    CheckedInt<size_t> newOffset = mOffset;
    newOffset += sizeof(uint32_t);
    AssertInBounds(newOffset);
    char* begin =
        reinterpret_cast<char*>(&mBuffer.Elements()[newOffset.value()]);

    // Next, ensure the buffer has space for enough bytes up to the size hint.
    // We may write fewer bytes than hinted, but we need to handle the worst
    // case if needed.
    newOffset += aSizeHint;
    AssertInBounds(newOffset);

    // Call the closure to write the bytes directly into the buffer.
    size_t bytesWritten = aClosure(Span(begin, aSizeHint));

    // Great, now we know the real length! Write it at the beginning.
    uint32_t lengthPrefix = PR_htonl(bytesWritten);
    memcpy(&mBuffer.Elements()[mOffset.value()], &lengthPrefix,
           sizeof(uint32_t));

    // And figure out our actual offset.
    newOffset -= aSizeHint;
    newOffset += bytesWritten;
    AssertInBounds(newOffset);
    mOffset = newOffset;
  }

  RustBuffer ToRustBuffer() {
    auto size = static_cast<int32_t>(mOffset.value());
    ForeignBytes bytes = {size, mBuffer.Elements()};
    RustError err = {0, nullptr};
    RustBuffer buffer = {{ ci.ffi_rustbuffer_from_bytes().name() }}(bytes, &err);
    if (err.mCode) {
      MOZ_ASSERT(false, "Failed to copy serialized data into Rust buffer");
    }
    return buffer;
  }

 private:
  void AssertInBounds(const CheckedInt<size_t>& aNewOffset) const {
    MOZ_RELEASE_ASSERT(aNewOffset.isValid() &&
                       aNewOffset.value() <= mBuffer.Capacity());
  }

  template <typename T>
  void WriteAt(const T& aValue,
               const std::function<void(size_t, const T&)>& aClosure) {
    CheckedInt<size_t> newOffset = mOffset;
    newOffset += sizeof(T);
    AssertInBounds(newOffset);
    mBuffer.SetLength(newOffset.value());
    aClosure(mOffset.value(), aValue);
    mOffset = newOffset;
  }

  nsTArray<uint8_t> mBuffer;
  CheckedInt<size_t> mOffset;
};

/// A "trait" struct with specializations for types that can be read and
/// written into a byte buffer. This struct is specialized for all serializable
/// types.
template <typename T>
struct Serializable {
  /// Returns the size of the serialized value, in bytes. This is used to
  /// calculate the allocation size for the Rust byte buffer.
  static CheckedInt<size_t> Size(const T& aValue) = delete;

  /// Reads a value of type `T` from a byte buffer.
  static bool ReadFrom(Reader& aReader, T& aValue) = delete;

  /// Writes a value of type `T` into a byte buffer.
  static void WriteInto(Writer& aWriter, const T& aValue) = delete;
};

/// A "trait" with specializations for types that can be transferred back and
/// forth over the FFI. This is analogous to the Rust trait of the same name.
/// As above, this gives us compile-time type checking for type pairs. If
/// `ViaFfi<T, U>::Lift(U, T)` compiles, we know that a value of type `U` from
/// the FFI can be lifted into a value of type `T`.
template <typename T, typename FfiType>
struct ViaFfi {
  static bool Lift(const FfiType& aLowered, T& aLifted) = delete;
  static FfiType Lower(const T& aLifted) = delete;
};

// This macro generates boilerplate specializations for primitive numeric types
// that are passed directly over the FFI without conversion.
#define UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(Type, readFunc, writeFunc)  \
  template <>                                                                \
  struct Serializable<Type> {                                                \
    static CheckedInt<size_t> Size(const Type& aValue) {                     \
      return sizeof(Type);                                                   \
    }                                                                        \
    [[nodiscard]] static bool ReadFrom(Reader& aReader, Type& aValue) {      \
      aValue = aReader.readFunc();                                           \
      return true;                                                           \
    }                                                                        \
    static void WriteInto(Writer& aWriter, const Type& aValue) {             \
      aWriter.writeFunc(aValue);                                             \
    }                                                                        \
  };                                                                         \
  template <>                                                                \
  struct ViaFfi<Type, Type> {                                                \
    [[nodiscard]] static bool Lift(const Type& aLowered, Type& aLifted) {    \
      aLifted = aLowered;                                                    \
      return true;                                                           \
    }                                                                        \
    [[nodiscard]] static Type Lower(const Type& aLifted) { return aLifted; } \
  }

UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(uint8_t, ReadUInt8, WriteUInt8);
UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(int8_t, ReadInt8, WriteInt8);
UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(uint16_t, ReadUInt16, WriteUInt16);
UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(int16_t, ReadInt16, WriteInt16);
UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(uint32_t, ReadUInt32, WriteUInt32);
UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(int32_t, ReadInt32, WriteInt32);
UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(uint64_t, ReadUInt64, WriteUInt64);
UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(int64_t, ReadInt64, WriteInt64);
UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(float, ReadFloat, WriteFloat);
UNIFFI_SPECIALIZE_SERIALIZABLE_PRIMITIVE(double, ReadDouble, WriteDouble);

/// Booleans are passed as unsigned integers over the FFI, because JNA doesn't
/// handle `bool`s well.

template <>
struct Serializable<bool> {
  static CheckedInt<size_t> Size(const bool& aValue) { return 1; }
  [[nodiscard]] static bool ReadFrom(Reader& aReader, bool& aValue) {
    aValue = aReader.ReadUInt8() != 0;
    return true;
  }
  static void WriteInto(Writer& aWriter, const bool& aValue) {
    aWriter.WriteUInt8(aValue ? 1 : 0);
  }
};

template <>
struct ViaFfi<bool, int8_t> {
  [[nodiscard]] static bool Lift(const int8_t& aLowered, bool& aLifted) {
    aLifted = aLowered != 0;
    return true;
  }
  [[nodiscard]] static int8_t Lower(const bool& aLifted) {
    return aLifted ? 1 : 0;
  }
};

/// Strings are length-prefixed and UTF-8 encoded when serialized
/// into byte buffers, and are passed as UTF-8 encoded `ForeignBytes`s over
/// the FFI.
///
/// Gecko has two string types: `nsCString` for "narrow" strings, and `nsString`
/// for "wide" strings. `nsCString`s don't have a fixed encoding: these can be
/// ASCII, Latin-1, or UTF-8. `nsString`s are always UTF-16. JS prefers
/// `nsString` (UTF-16; also called `DOMString` in WebIDL); `nsCString`s
/// (`ByteString` in WebIDL) are pretty uncommon.
///
/// `nsCString`s can be passed to Rust directly, and copied byte-for-byte into
/// buffers. The UniFFI scaffolding code will ensure they're valid UTF-8. But
/// `nsString`s must be converted to UTF-8 first.

template <>
struct Serializable<nsACString> {
  static CheckedInt<size_t> Size(const nsACString& aValue) {
    CheckedInt<size_t> size(aValue.Length());
    size += sizeof(uint32_t);  // For the length prefix.
    return size;
  }

  [[nodiscard]] static bool ReadFrom(Reader& aReader, nsACString& aValue) {
    aValue.Truncate();
    aReader.ReadRawString<nsACString>(
        aValue, [](Span<const char> aRawString, nsACString& aValue) {
          aValue.Append(aRawString);
        });
    return true;
  }

  static void WriteInto(Writer& aWriter, const nsACString& aValue) {
    aWriter.WriteRawString(aValue.Length(), [&](Span<char> aRawString) {
      memcpy(aRawString.Elements(), aValue.BeginReading(), aRawString.Length());
      return aRawString.Length();
    });
  }
};

template <>
struct ViaFfi<nsACString, RustBuffer> {
  [[nodiscard]] static bool Lift(const RustBuffer& aLowered,
                                 nsACString& aLifted) {
    aLifted.Truncate();
    if (aLowered.mData) {
      aLifted.Append(AsChars(Span(aLowered.mData, aLowered.mLen)));
      RustError err = {0, nullptr};
      {{ ci.ffi_rustbuffer_free().name() }}(aLowered, &err);
      if (err.mCode) {
        MOZ_ASSERT(false, "Failed to lift `nsACString` from Rust buffer");
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static RustBuffer Lower(const nsACString& aLifted) {
    RustError err = {0, nullptr};
    ForeignBytes bytes = {
        static_cast<int32_t>(aLifted.Length()),
        reinterpret_cast<const uint8_t*>(aLifted.BeginReading())};
    RustBuffer lowered = {{ ci.ffi_rustbuffer_from_bytes().name() }}(bytes, &err);
    if (err.mCode) {
      MOZ_ASSERT(false, "Failed to lower `nsACString` into Rust string");
    }
    return lowered;
  }
};

/// Shared traits for serializing `nsString`s and `nsAString`s.
template <typename T>
struct StringTraits {
  static CheckedInt<size_t> Size(const T& aValue) {
    auto size = EstimateUTF8Length(aValue);
    size += sizeof(uint32_t);  // For the length prefix.
    return size;
  }

  [[nodiscard]] static bool ReadFrom(Reader& aReader, T& aValue) {
    aValue.Truncate();
    aReader.ReadRawString<T>(aValue,
                             [](Span<const char> aRawString, T& aValue) {
                               AppendUTF8toUTF16(aRawString, aValue);
                             });
    return true;
  }

  static void WriteInto(Writer& aWriter, const T& aValue) {
    auto length = EstimateUTF8Length(aValue);
    MOZ_RELEASE_ASSERT(length.isValid());
    aWriter.WriteRawString(length.value(), [&](Span<char> aRawString) {
      return ConvertUtf16toUtf8(aValue, aRawString);
    });
  }

  /// Estimates the UTF-8 encoded length of a UTF-16 string. This is a
  /// worst-case estimate.
  static CheckedInt<size_t> EstimateUTF8Length(const T& aUTF16) {
    CheckedInt<size_t> length(aUTF16.Length());
    // `ConvertUtf16toUtf8` expects the destination to have at least three times
    // as much space as the source string.
    length *= 3;
    return length;
  }
};

template <>
struct Serializable<nsAString> {
  static CheckedInt<size_t> Size(const nsAString& aValue) {
    return StringTraits<nsAString>::Size(aValue);
  }

  [[nodiscard]] static bool ReadFrom(Reader& aReader, nsAString& aValue) {
    return StringTraits<nsAString>::ReadFrom(aReader, aValue);
  }

  static void WriteInto(Writer& aWriter, const nsAString& aValue) {
    StringTraits<nsAString>::WriteInto(aWriter, aValue);
  }
};

template <>
struct ViaFfi<nsAString, RustBuffer> {
  [[nodiscard]] static bool Lift(const RustBuffer& aLowered,
                                 nsAString& aLifted) {
    aLifted.Truncate();
    if (aLowered.mData) {
      CopyUTF8toUTF16(AsChars(Span(aLowered.mData, aLowered.mLen)), aLifted);
      RustError err = {0, nullptr};
      {{ ci.ffi_rustbuffer_free().name() }}(aLowered, &err);
      if (err.mCode) {
        MOZ_ASSERT(false, "Failed to lift `nsAString` from Rust buffer");
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static RustBuffer Lower(const nsAString& aLifted) {
    // Encode the string to UTF-8, then make a Rust string from the contents.
    // This copies the string twice, but is safe.
    nsAutoCString utf8;
    CopyUTF16toUTF8(aLifted, utf8);
    ForeignBytes bytes = {
        static_cast<int32_t>(utf8.Length()),
        reinterpret_cast<const uint8_t*>(utf8.BeginReading())};
    RustError err = {0, nullptr};
    RustBuffer lowered = {{ ci.ffi_rustbuffer_from_bytes().name() }}(bytes, &err);
    if (err.mCode) {
      MOZ_ASSERT(false, "Failed to lower `nsAString` into Rust string");
    }
    return lowered;
  }
};

template <>
struct Serializable<nsString> {
  static CheckedInt<size_t> Size(const nsString& aValue) {
    return StringTraits<nsString>::Size(aValue);
  }

  [[nodiscard]] static bool ReadFrom(Reader& aReader, nsString& aValue) {
    return StringTraits<nsString>::ReadFrom(aReader, aValue);
  }

  static void WriteInto(Writer& aWriter, const nsString& aValue) {
    StringTraits<nsString>::WriteInto(aWriter, aValue);
  }
};

/// Nullable values are prefixed by a tag: 0 if none; 1 followed by the
/// serialized value if some. These are turned into Rust `Option<T>`s.
///
/// Fun fact: WebIDL also has a `dom::Optional<T>` type. They both use
/// `mozilla::Maybe<T>` under the hood, but their semantics are different.
/// `Nullable<T>` means JS must pass some value for the argument or dictionary
/// field: either `T` or `null`. `Optional<T>` means JS can omit the argument
/// or member entirely.
///
/// These are always serialized, never passed directly over the FFI.

template <typename T>
struct Serializable<dom::Nullable<T>> {
  static CheckedInt<size_t> Size(const dom::Nullable<T>& aValue) {
    if (aValue.IsNull()) {
      return 1;
    }
    CheckedInt<size_t> size(1);
    size += Serializable<T>::Size(aValue.Value());
    return size;
  }

  [[nodiscard]] static bool ReadFrom(Reader& aReader, dom::Nullable<T>& aValue) {
    uint8_t hasValue = aReader.ReadUInt8();
    if (hasValue != 0 && hasValue != 1) {
      return false;
    }
    if (!hasValue) {
      aValue = dom::Nullable<T>();
      return true;
    }
    T value;
    if (!Serializable<T>::ReadFrom(aReader, value)) {
      return false;
    }
    aValue = dom::Nullable<T>(std::move(value));
    return true;
  };

  static void WriteInto(Writer& aWriter, const dom::Nullable<T>& aValue) {
    if (aValue.IsNull()) {
      aWriter.WriteUInt8(0);
    } else {
      aWriter.WriteUInt8(1);
      Serializable<T>::WriteInto(aWriter, aValue.Value());
    }
  }
};

/// Sequences are length-prefixed, followed by the serialization of each
/// element. They're always serialized, and never passed directly over the
/// FFI.
///
/// WebIDL has two different representations for sequences, though they both
/// use `nsTArray<T>` under the hood. `dom::Sequence<T>` is for sequence
/// arguments; `nsTArray<T>` is for sequence return values and dictionary
/// members.

/// Shared traits for serializing sequences.
template <typename T>
struct SequenceTraits {
  static CheckedInt<size_t> Size(const T& aValue) {
    CheckedInt<size_t> size;
    size += sizeof(int32_t);  // For the length prefix.
    for (const typename T::elem_type& element : aValue) {
      size += Serializable<typename T::elem_type>::Size(element);
    }
    return size;
  }

  static void WriteInto(Writer& aWriter, const T& aValue) {
    aWriter.WriteUInt32(aValue.Length());
    for (const typename T::elem_type& element : aValue) {
      Serializable<typename T::elem_type>::WriteInto(aWriter, element);
    }
  }
};

template <typename T>
struct Serializable<dom::Sequence<T>> {
  static CheckedInt<size_t> Size(const dom::Sequence<T>& aValue) {
    return SequenceTraits<dom::Sequence<T>>::Size(aValue);
  }

  // We leave `ReadFrom` unimplemented because sequences should only be
  // lowered from the C++ WebIDL binding to the FFI. If the FFI function
  // returns a sequence, it'll be lifted into an `nsTArray<T>`, not a
  // `dom::Sequence<T>`. See the note about sequences above.
  [[nodiscard]] static bool ReadFrom(Reader& aReader,
                                    dom::Sequence<T>& aValue) = delete;

  static void WriteInto(Writer& aWriter, const dom::Sequence<T>& aValue) {
    SequenceTraits<dom::Sequence<T>>::WriteInto(aWriter, aValue);
  }
};

template <typename T>
struct Serializable<nsTArray<T>> {
  static CheckedInt<size_t> Size(const nsTArray<T>& aValue) {
    return SequenceTraits<nsTArray<T>>::Size(aValue);
  }

  [[nodiscard]] static bool ReadFrom(Reader& aReader, nsTArray<T>& aValue) {
    uint32_t length = aReader.ReadUInt32();
    aValue.SetCapacity(length);
    aValue.TruncateLength(0);
    for (uint32_t i = 0; i < length; ++i) {
      if (!Serializable<T>::ReadFrom(aReader, *aValue.AppendElement())) {
        return false;
      }
    }
    return true;
  };

  static void WriteInto(Writer& aWriter, const nsTArray<T>& aValue) {
    SequenceTraits<nsTArray<T>>::WriteInto(aWriter, aValue);
  }
};

template <typename K, typename V>
struct Serializable<Record<K, V>> {
  static CheckedInt<size_t> Size(const Record<K, V>& aValue) {
    CheckedInt<size_t> size;
    size += sizeof(uint32_t);  // For the length prefix.
    for (const typename Record<K, V>::EntryType& entry : aValue.Entries()) {
      size += Serializable<K>::Size(entry.mKey);
      size += Serializable<V>::Size(entry.mValue);
    }
    return size;
  }

  [[nodiscard]] static bool ReadFrom(Reader& aReader, Record<K, V>& aValue) {
    uint32_t length = aReader.ReadUInt32();
    aValue.Entries().SetCapacity(length);
    aValue.Entries().TruncateLength(0);
    for (uint32_t i = 0; i < length; ++i) {
      typename Record<K, V>::EntryType* entry =
          aValue.Entries().AppendElement();
      if (!Serializable<K>::ReadFrom(aReader, entry->mKey)) {
        return false;
      }
      if (!Serializable<V>::ReadFrom(aReader, entry->mValue)) {
        return false;
      }
    }
    return true;
  };

  static void WriteInto(Writer& aWriter, const Record<K, V>& aValue) {
    aWriter.WriteUInt32(aValue.Entries().Length());
    for (const typename Record<K, V>::EntryType& entry : aValue.Entries()) {
      Serializable<K>::WriteInto(aWriter, entry.mKey);
      Serializable<V>::WriteInto(aWriter, entry.mValue);
    }
  }
};

/// Partial specialization for all types that can be serialized into a byte
/// buffer. This is analogous to the `ViaFfiUsingByteBuffer` trait in Rust.

template <typename T>
struct ViaFfi<T, RustBuffer> {
  [[nodiscard]] static bool Lift(const RustBuffer& aLowered, T& aLifted) {
    auto reader = Reader(aLowered);
    if (!Serializable<T>::ReadFrom(reader, aLifted)) {
      return false;
    }
    if (reader.HasRemaining()) {
      MOZ_ASSERT(false);
      return false;
    }
    RustError err = {0, nullptr};
    {{ ci.ffi_rustbuffer_free().name() }}(aLowered, &err);
    if (err.mCode) {
      MOZ_ASSERT(false, "Failed to free Rust buffer after lifting contents");
      return false;
    }
    return true;
  }

  [[nodiscard]] static RustBuffer Lower(const T& aLifted) {
    CheckedInt<size_t> size = Serializable<T>::Size(aLifted);
    MOZ_RELEASE_ASSERT(size.isValid());
    auto writer = Writer(size.value());
    Serializable<T>::WriteInto(writer, aLifted);
    return writer.ToRustBuffer();
  }
};

}  // namespace {{ ci.namespace()|detail_cpp }}
