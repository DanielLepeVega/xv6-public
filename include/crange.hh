#pragma once

struct crange;
struct crange_locked;
struct range;
class range_iterator;

template<class T>
class markptr_ptr;

template<class T>
class markptr_mark;

template<class T>
class markptr {
 protected:
  std::atomic<uptr> _p;

 public:
  markptr() : _p(0) {}
  markptr(T* v) : _p((uptr) v) {}
  markptr(const markptr<T> &v) : _p(v._p.load()) {}

  void operator=(T* v) { _p = (uptr) v; }
  void operator=(const markptr<T> &v) { _p = v._p.load(); }

  bool operator!=(const markptr<T> &v) const { return _p != v._p; }
  bool operator==(const markptr<T> &v) const { return _p == v._p; }

  markptr_ptr<T>& ptr() {
    return *(markptr_ptr<T>*) this;
  }

  markptr_mark<T>& mark() {
    return *(markptr_mark<T>*) this;
  }

  // Convenience operator to avoid having to write out xx.ptr()->...
  T* operator->() { return ptr(); }

  bool cmpxch(markptr<T> expected, markptr<T> desired) {
    uptr ee = expected._p.load();
    return _p.compare_exchange_weak(ee, desired._p.load());
  }
};

template<class T>
class markptr_ptr : private markptr<T> {
 public:
  void operator=(T *p) {
    uptr p0, p1;
    do {
      p0 = markptr<T>::_p.load();
      p1 = (p0 & 1) | (uptr) p;
    } while (!markptr<T>::_p.compare_exchange_weak(p0, p1));
  }

  T* load() const {
    return (T*) (markptr<T>::_p.load() & ~1);
  }

  operator T*() const { return load(); }
};

template<class T>
class markptr_mark : public markptr<T> {
 public:
  void operator=(bool m) {
    uptr p0, p1;
    do {
      p0 = markptr<T>::_p.load();
      p1 = (p0 & ~1) | !!m;
    } while (!markptr<T>::_p.compare_exchange_weak(p0, p1));
  }

  bool load() const {
    return markptr<T>::_p.load() & 1;
  }

  operator bool() const { return load(); }
};

struct range : public rcu_freed {
 private:
  const u64 key;
  const u64 size;
  std::atomic<int> curlevel; // the current levels it appears on
  const int nlevel;      // the number of levels this range should appear
  crange *const cr;      // the crange this range is part of
  markptr<range>* const next; // one next pointer per level
  spinlock lock;         // on separate cache line?

  void print(int l);
  void dec_ref(void);
  int lockif(markptr<range> e);

  friend class crange;
  friend class crange_locked;
  friend class range_iterator;

 protected:
  ~range();
  range(crange *cr, u64 k, u64 sz, int nlevel = 0);

 public:
  bool deleted() { return next[0].mark(); }
} __mpalign__;

struct range_head : public range {
  range_head(crange *cr, u64 k, u64 sz, int nlevel)
    : range(cr, k, sz, nlevel) {}

  NEW_DELETE_OPS(range_head)

  virtual void do_gc() {
    delete this;
  }
};

class range_iterator {
 private:
  range *_e;

 public:
  range_iterator(range *e) : _e(e) {}
  range_iterator& operator++() { _e = _e->next[0].ptr(); return *this; }

  range*& operator*() { return _e; }
  bool operator==(const range_iterator &other) { return _e == other._e; }
  bool operator!=(const range_iterator &other) { return _e != other._e; }
};

struct crange_locked {
 private:
  crange *const cr_;
  const u64 base_, size_;

  range *const prev_;
  range *const succ_;

  bool moved_;
  scoped_gc_epoch gc;

  crange_locked(crange *cr, u64 base, u64 sz, range *p, range *s)
    : cr_(cr), base_(base), size_(sz), prev_(p), succ_(s), moved_(false) {}
  crange_locked(const crange_locked&) = delete;
  crange_locked& operator=(const crange_locked&) = delete;
  friend class crange;

 public:
  crange_locked(crange_locked &&x)
    : cr_(x.cr_),
      base_(x.base_),
      size_(x.size_),
      prev_(x.prev_),
      succ_(x.succ_),
      moved_(false),
      gc(std::move(x.gc))
  {
    x.moved_ = true;
  }

  ~crange_locked() {
    if (!moved_) {
      range *n;
      for (range *e = prev_; e && e != succ_; e = n) {
        // as soon a we release, the next pointer can change, so read it first
        n = e->next[0].ptr();
        release(&e->lock);
      }
    }
  }

  range_iterator begin() const { return range_iterator(prev_->next[0].ptr()); };
  range_iterator end() const { return range_iterator(succ_); };
  void replace(range *r);
  void replace(range *prev, range *repl);
};

struct crange {
 private:
  const int nlevel;            // number of levels in the crange skip list
  range_head *const crange_head; // a crange skip list starts with a sentinel range (key 0, sz 0)

  static void mark(range *f, range *s);
  static int lock_range(u64 k, u64 sz, int l, range **er, range **pr, range **fr, range **lr, range **sr);

  void print(int);
  void check(struct range *absent);
  int del_index(range *p0, range **e, int l);
  void add_index(int l, range *e, range *p1, markptr<range> s1);
  int find_and_lock(u64 k, u64 sz, range **p0, range **f0, range **l0, range **s0);

  friend class crange_locked;
  friend class range;

 public:
  crange(int nlevel);
  ~crange(void);

  range* search(u64 k, u64 sz);
  crange_locked search_lock(u64 k, u64 sz) {
    range *prev, *first, *last, *succ;
    find_and_lock(k, sz, &prev, &first, &last, &succ);
    return crange_locked(this, k, sz, prev, succ);
  };

  range_iterator begin() const { return range_iterator(crange_head->next[0].ptr()); };
  range_iterator end() const { return range_iterator(0); };
  NEW_DELETE_OPS(crange)
};

static inline range_iterator
begin(const crange &cr)
{
  return cr.begin();
}

static inline range_iterator
end(const crange &cr)
{
  return cr.end();
}

static inline range_iterator
begin(const crange_locked &crl)
{
  return crl.begin();
}

static inline range_iterator
end(const crange_locked &crl)
{
  return crl.end();
}