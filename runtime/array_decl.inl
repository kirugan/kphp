#pragma once

#include "runtime/include.h"

#ifndef INCLUDED_FROM_KPHP_CORE
  #error "this file must be included only from kphp_core.h"
#endif

struct array_size {
  int int_size;
  int string_size;
  bool is_vector;

  inline array_size(int int_size, int string_size, bool is_vector);

  inline array_size operator+(const array_size &other) const;

  inline array_size &cut(int length);

  inline array_size &min(const array_size &other);
};

namespace dl {
template<class T, class TT, class T1>
void sort(TT *begin_init, TT *end_init, const T1 &compare);
}

template<class T>
class array : array_tag {

public:
  typedef var key_type;

  inline static bool is_int_key(const key_type &key);

private:

  using entry_pointer_type = dl::size_type;

  struct list_hash_entry {
    entry_pointer_type next;
    entry_pointer_type prev;
  };

  struct int_hash_entry : list_hash_entry {
    T value;

    int int_key;

    inline key_type get_key() const;
  };

  struct string_hash_entry : list_hash_entry {
    T value;

    int int_key;
    string string_key;

    inline key_type get_key() const;
  };

  struct array_inner {
    //if key is number, int_key contains this number, there is no string_key.
    //if key is string, int_key contains hash of this string, string_key contains this string.
    //empty hash_entry identified by (next == EMPTY_POINTER)
    //vector is_identified by string_buf_size == -1

    static const int MAX_HASHTABLE_SIZE = (1 << 26);
    static const int MIN_HASHTABLE_SIZE = 1;
    static const int DEFAULT_HASHTABLE_SIZE = (1 << 3);

    static const entry_pointer_type EMPTY_POINTER;
    static const T empty_T;

    int ref_cnt;
    int max_key;
    list_hash_entry end_;
    int int_size;
    int int_buf_size;
    int string_size;
    int string_buf_size;
    int_hash_entry int_entries[0];

    inline bool is_vector() const __attribute__ ((always_inline));

    inline list_hash_entry *get_entry(entry_pointer_type pointer) const __attribute__ ((always_inline));
    inline entry_pointer_type get_pointer(list_hash_entry *entry) const __attribute__ ((always_inline));

    inline const string_hash_entry *begin() const __attribute__ ((always_inline));
    inline const string_hash_entry *next(const string_hash_entry *p) const __attribute__ ((always_inline));
    inline const string_hash_entry *prev(const string_hash_entry *p) const __attribute__ ((always_inline));
    inline const string_hash_entry *end() const __attribute__ ((always_inline));

    inline string_hash_entry *begin() __attribute__ ((always_inline));
    inline string_hash_entry *next(string_hash_entry *p) __attribute__ ((always_inline));
    inline string_hash_entry *prev(string_hash_entry *p) __attribute__ ((always_inline));
    inline string_hash_entry *end() __attribute__ ((always_inline));

    inline bool is_string_hash_entry(const string_hash_entry *p) const __attribute__ ((always_inline));
    inline const string_hash_entry *get_string_entries() const __attribute__ ((always_inline));
    inline string_hash_entry *get_string_entries() __attribute__ ((always_inline));

    inline static int choose_bucket(const int key, const int buf_size) __attribute__ ((always_inline));

    inline static array_inner *create(int new_int_size, int new_string_size, bool is_vector);

    inline static array_inner *empty_array() __attribute__ ((always_inline));

    inline void dispose() __attribute__ ((always_inline));

    inline array_inner *ref_copy() __attribute__ ((always_inline));

    inline const var get_var(int int_key) const;
    inline const T get_value(int int_key) const;
    inline T &push_back_vector_value(const T &v) /*__attribute__ ((always_inline))*/;//unsafe //TODO receive T
    inline T &set_vector_value(int int_key, const T &v) /*__attribute__ ((always_inline))*/;//unsafe
    inline T &set_map_value(int int_key, const T &v, bool save_value) /*__attribute__ ((always_inline))*/;
    inline bool has_key(int int_key) const;
    inline bool isset_value(int int_key) const;
    inline void unset_vector_value();
    inline void unset_map_value(int int_key);

    inline const var get_var(int int_key, const string &string_key) const;
    inline const T get_value(int int_key, const string &string_key) const;
    inline const T &get_vector_value(int int_key) const;//unsafe
    inline T &get_vector_value(int int_key);//unsafe
    inline T &set_map_value(int int_key, const string &string_key, const T &v, bool save_value) /*__attribute__ ((always_inline))*/;
    inline bool has_key(int int_key, const string &string_key) const;
    inline bool isset_value(int int_key, const string &string_key) const;
    inline void unset_map_value(int int_key, const string &string_key);

    inline array_inner() = delete;
    inline array_inner(const array_inner &other) = delete;
    inline array_inner &operator=(const array_inner &other) = delete;
  };

  inline bool mutate_if_vector_shared(int mul = 1);
  inline bool mutate_to_size_if_vector_shared(int int_size);
  inline void mutate_to_size(int int_size);
  inline bool mutate_if_map_shared(int mul = 1);
  inline void mutate_if_vector_needed_int();
  inline void mutate_if_map_needed_int();
  inline void mutate_if_map_needed_string();

  inline void convert_to_map();

  template<class T1>
  inline void copy_from(const array<T1> &other);

  inline void destroy() __attribute__ ((always_inline));

public:

  class const_iterator {
  private:
    const array_inner *self;
    const list_hash_entry *entry;
  public:
    inline const_iterator() __attribute__ ((always_inline));
    inline const_iterator(const array_inner *self, const list_hash_entry *entry) __attribute__ ((always_inline));

    inline const T &get_value() const __attribute__ ((always_inline));
    inline key_type get_key() const __attribute__ ((always_inline));
    inline const_iterator &operator++() __attribute__ ((always_inline));
    inline const_iterator &operator--() __attribute__ ((always_inline));
    inline bool operator==(const const_iterator &other) const __attribute__ ((always_inline));
    inline bool operator!=(const const_iterator &other) const __attribute__ ((always_inline));

    template<class T1>
    friend class array;
  };

  class iterator {
  private:
    array_inner *self;
    list_hash_entry *entry;
  public:
    inline iterator() __attribute__ ((always_inline));
    inline iterator(array_inner *self, list_hash_entry *entry) __attribute__ ((always_inline));

    inline T &get_value() __attribute__ ((always_inline));
    inline key_type get_key() const __attribute__ ((always_inline));
    inline iterator &operator++() __attribute__ ((always_inline));
    inline iterator &operator--() __attribute__ ((always_inline));
    inline bool operator==(const iterator &other) const __attribute__ ((always_inline));
    inline bool operator!=(const iterator &other) const __attribute__ ((always_inline));

    template<class T1>
    friend
    class array;
  };

  inline array() __attribute__ ((always_inline));

  inline explicit array(const array_size &s) __attribute__ ((always_inline));

  template<class... Args, typename std::enable_if<sizeof...(Args) >= 2>::type * = nullptr>
  inline array(Args &&... args) __attribute__ ((always_inline));

  template<class KeyT>
  inline array(const std::initializer_list<std::pair<KeyT, T>> &list) __attribute__ ((always_inline));

  inline array(const array &other) __attribute__ ((always_inline));

  inline array(array &&other) noexcept __attribute__ ((always_inline));

  template<class T1, class = enable_if_constructible_or_unknown<T, T1>>
  inline array(const array<T1> &other) __attribute__ ((always_inline));

  inline array &operator=(const array &other) __attribute__ ((always_inline));

  inline array &operator=(array &&other) noexcept __attribute__ ((always_inline));

  template<class T1, class = enable_if_constructible_or_unknown<T, T1>>
  inline array &operator=(const array<T1> &other) __attribute__ ((always_inline));

  inline ~array() /*__attribute__ ((always_inline))*/;

  inline void clear() __attribute__ ((always_inline));

  inline bool is_vector() const __attribute__ ((always_inline));

  T &operator[](int int_key);
  T &operator[](const string &s);
  T &operator[](const var &v);
  T &operator[](const const_iterator &it);
  T &operator[](const iterator &it);

  void set_value(int int_key, const T &v);
  void set_value(const string &s, const T &v);
  void set_value(const string &s, const T &v, int precomuted_hash);
  void set_value(const var &v, const T &value);

  template<class OrFalseT>
  void set_value(const OrFalse<OrFalseT> &key, const T &value);

  void set_value(const const_iterator &it);
  void set_value(const iterator &it);

  // assign binary array_inner representation
  // can be used only on empty arrays to receive logically const array
  void assign_raw(const char *s);

  const var get_var(int int_key) const;
  const var get_var(const string &s) const;
  const var get_var(const var &v) const;

  const T get_value(int int_key) const;
  const T get_value(const string &s) const;
  const T get_value(const string &s, int precomuted_hash) const;
  const T get_value(const var &v) const;
  const T get_value(const const_iterator &it) const;
  const T get_value(const iterator &it) const;

  void push_back(const T &v);
  void push_back(const const_iterator &it);
  void push_back(const iterator &it);
  const T push_back_return(const T &v);

  inline void fill_vector(int num, const T &value);

  inline int get_next_key() const __attribute__ ((always_inline));

  bool has_key(int int_key) const;
  bool has_key(const string &s) const;
  bool has_key(const var &v) const;
  bool has_key(const const_iterator &it) const;
  bool has_key(const iterator &it) const;

  bool isset(int int_key) const;
  bool isset(const string &s) const;
  bool isset(const var &v) const;
  bool isset(const const_iterator &it) const;
  bool isset(const iterator &it) const;

  void unset(int int_key);
  void unset(const string &s);
  void unset(const var &v);
  void unset(const const_iterator &it);
  void unset(const iterator &it);

  inline bool empty() const __attribute__ ((always_inline));
  inline int count() const __attribute__ ((always_inline));

  inline array_size size() const __attribute__ ((always_inline));

  template<class T1, class = enable_if_constructible_or_unknown<T, T1>>
  void merge_with(const array<T1> &other);

  const array operator+(const array &other) const;
  array &operator+=(const array &other);

  inline const_iterator begin() const __attribute__ ((always_inline));
  inline const_iterator middle(int n) const __attribute__ ((always_inline));
  inline const_iterator end() const __attribute__ ((always_inline));

  inline iterator begin() __attribute__ ((always_inline));
  inline iterator middle(int n) __attribute__ ((always_inline));
  inline iterator end() __attribute__ ((always_inline));

  template<class T1>
  void sort(const T1 &compare, bool renumber);

  template<class T1>
  void ksort(const T1 &compare);

  inline void swap(array &other) __attribute__ ((always_inline));


  T pop();

  T shift();

  int unshift(const T &val);


  inline bool to_bool() const __attribute__ ((always_inline));
  inline int to_int() const __attribute__ ((always_inline));
  inline double to_float() const __attribute__ ((always_inline));


  int get_reference_counter() const;
  void set_reference_counter_to_const();

  const T *get_const_vector_pointer() const; // unsafe

  void reserve(int int_size, int string_size, bool make_vector_if_possible);

  template<typename U>
  static array<T> convert_from(const array<U> &);
private:
  void push_back_values() {}

  template<class Arg, class...Args>
  void push_back_values(Arg &&arg, Args &&... args);

private:
  array_inner *p;

  template<class T1>
  friend class array;

  friend class var;
};

template<class T>
inline void swap(array<T> &lhs, array<T> &rhs);

template<class T>
inline const array<T> array_add(array<T> a1, const array<T> &a2);

template<class T>
inline bool eq2(const array<T> &lhs, const array<T> &rhs);

template<class T1, class T2>
inline bool eq2(const array<T1> &lhs, const array<T2> &rhs);

template<class T, class TT>
inline bool equals(const array<T> &lhs, const array<T> &rhs);

template<class T1, class T2>
inline bool equals(const array<T1> &lhs, const array<T2> &rhs);