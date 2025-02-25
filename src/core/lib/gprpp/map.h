/*
 *
 * Copyright 2017 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef GRPC_CORE_LIB_GPRPP_MAP_H
#define GRPC_CORE_LIB_GPRPP_MAP_H

#include <grpc/support/port_platform.h>

#include <string.h>

#include <algorithm>
#include <functional>
#include <iterator>

#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/gprpp/pair.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"

namespace grpc_core {
struct StringLess {
  bool operator()(const char* a, const char* b) const {
    return strcmp(a, b) < 0;
  }
  bool operator()(const UniquePtr<char>& k1, const UniquePtr<char>& k2) const {
    return strcmp(k1.get(), k2.get()) < 0;
  }
};

template <typename T>
struct RefCountedPtrLess {
  bool operator()(const RefCountedPtr<T>& p1,
                  const RefCountedPtr<T>& p2) const {
    return p1.get() < p2.get();
  }
};

namespace testing {
class MapTest;
}

// Alternative map implementation for grpc_core
template <class Key, class T, class Compare = std::less<Key>>
class Map {
 public:
  typedef Key key_type;
  typedef T mapped_type;
  typedef Pair<key_type, mapped_type> value_type;
  typedef Compare key_compare;
  class iterator;
  class const_iterator;

  Map() = default;
  ~Map() { clear(); }

  // Movable.
  Map(Map&& other) : root_(other.root_), size_(other.size_) {
    other.root_ = nullptr;
    other.size_ = 0;
  }
  Map& operator=(Map&& other) {
    if (this != &other) {
      clear();
      root_ = other.root_;
      size_ = other.size_;
      other.root_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }

  // Copyable.
  Map(const Map& other) {
    for (const auto& p : other) {
      emplace(p);
    }
  }
  Map& operator=(const Map& other) {
    if (this != &other) {
      clear();
      for (const auto& p : other) {
        emplace(p);
      }
    }
    return *this;
  }

  T& operator[](key_type&& key);
  T& operator[](const key_type& key);
  iterator find(const key_type& k);
  size_t erase(const key_type& key);
  // Removes the current entry and points to the next one
  iterator erase(iterator iter);

  size_t size() const { return size_; }

  template <class... Args>
  Pair<iterator, bool> emplace(Args&&... args);

  Pair<iterator, bool> insert(value_type&& pair) {
    return emplace(std::move(pair));
  }

  Pair<iterator, bool> insert(const value_type& pair) { return emplace(pair); }

  bool empty() const { return root_ == nullptr; }

  void clear() {
    auto iter = begin();
    while (!empty()) {
      iter = erase(iter);
    }
  }

  iterator begin() {
    Entry* curr = GetMinEntry(root_);
    return iterator(this, curr);
  }

  iterator end() { return iterator(this, nullptr); }

  const_iterator begin() const {
    Entry* curr = GetMinEntry(root_);
    return const_iterator(this, curr);
  }

  const_iterator end() const { return const_iterator(this, nullptr); }

  iterator lower_bound(const Key& k) {
    // This is a workaround for "const key_compare compare;"
    // because some versions of compilers cannot build this by requiring
    // a user-provided constructor. (ref: https://stackoverflow.com/q/7411515)
    key_compare compare_tmp;
    const key_compare& compare = compare_tmp;
    return std::find_if(begin(), end(), [&k, &compare](const value_type& v) {
      return !compare(v.first, k);
    });
  }

 private:
  friend class testing::MapTest;
  struct Entry {
    explicit Entry(value_type&& pair) : pair(std::move(pair)) {}
    value_type pair;
    Entry* left = nullptr;
    Entry* right = nullptr;
    int32_t height = 1;
  };

  static int32_t EntryHeight(const Entry* e) {
    return e == nullptr ? 0 : e->height;
  }

  static Entry* GetMinEntry(Entry* e);
  Entry* InOrderSuccessor(const Entry* e) const;
  static Entry* RotateLeft(Entry* e);
  static Entry* RotateRight(Entry* e);
  static Entry* RebalanceTreeAfterInsertion(Entry* root, const key_type& k);
  static Entry* RebalanceTreeAfterDeletion(Entry* root);
  // Returns a pair with the first value being an iterator pointing to the
  // inserted entry and the second value being the new root of the subtree
  // after a rebalance
  Pair<iterator, Entry*> InsertRecursive(Entry* root, value_type&& p);
  // Returns a pair with the first value being an iterator pointing to the
  // successor of the deleted entry and the second value being the new root of
  // the subtree after a rebalance
  Pair<iterator, Entry*> RemoveRecursive(Entry* root, const key_type& k);
  // Return 0 if lhs = rhs
  //        1 if lhs > rhs
  //       -1 if lhs < rhs
  static int CompareKeys(const Key& lhs, const Key& rhs);

  Entry* root_ = nullptr;
  size_t size_ = 0;
};

template <class Key, class T, class Compare>
class Map<Key, T, Compare>::iterator
    : public std::iterator<std::input_iterator_tag, Pair<Key, T>, int32_t,
                           Pair<Key, T>*, Pair<Key, T>&> {
 public:
  iterator(const iterator& iter) : curr_(iter.curr_), map_(iter.map_) {}
  bool operator==(const iterator& rhs) const { return (curr_ == rhs.curr_); }
  bool operator!=(const iterator& rhs) const { return (curr_ != rhs.curr_); }

  iterator& operator++() {
    curr_ = map_->InOrderSuccessor(curr_);
    return *this;
  }

  iterator operator++(int) {
    Entry* prev = curr_;
    curr_ = map_->InOrderSuccessor(curr_);
    return iterator(map_, prev);
  }

  iterator& operator=(const iterator& other) {
    if (this != &other) {
      this->curr_ = other.curr_;
      this->map_ = other.map_;
    }
    return *this;
  }

  // operator*()
  value_type& operator*() { return curr_->pair; }
  const value_type& operator*() const { return curr_->pair; }

  // operator->()
  value_type* operator->() { return &curr_->pair; }
  value_type const* operator->() const { return &curr_->pair; }

 private:
  friend class Map<key_type, mapped_type, key_compare>;
  using GrpcMap = typename ::grpc_core::Map<Key, T, Compare>;
  iterator(GrpcMap* map, Entry* curr) : curr_(curr), map_(map) {}
  Entry* curr_;
  GrpcMap* map_;
};

template <class Key, class T, class Compare>
class Map<Key, T, Compare>::const_iterator
    : public std::iterator<std::input_iterator_tag, Pair<Key, T>, int32_t,
                           Pair<Key, T>*, Pair<Key, T>&> {
 public:
  const_iterator(const const_iterator& iter)
      : curr_(iter.curr_), map_(iter.map_) {}
  bool operator==(const const_iterator& rhs) const {
    return (curr_ == rhs.curr_);
  }
  bool operator!=(const const_iterator& rhs) const {
    return (curr_ != rhs.curr_);
  }

  const_iterator& operator++() {
    curr_ = map_->InOrderSuccessor(curr_);
    return *this;
  }

  const_iterator operator++(int) {
    Entry* prev = curr_;
    curr_ = map_->InOrderSuccessor(curr_);
    return const_iterator(map_, prev);
  }

  const_iterator& operator=(const const_iterator& other) {
    if (this != &other) {
      this->curr_ = other.curr_;
      this->map_ = other.map_;
    }
    return *this;
  }

  // operator*()
  const value_type& operator*() const { return curr_->pair; }

  // operator->()
  const value_type* operator->() const { return &curr_->pair; }

 private:
  friend class Map<key_type, mapped_type, key_compare>;
  using GrpcMap = typename ::grpc_core::Map<Key, T, Compare>;
  const_iterator(const GrpcMap* map, Entry* curr) : curr_(curr), map_(map) {}
  Entry* curr_;
  const GrpcMap* map_;
};

template <class Key, class T, class Compare>
T& Map<Key, T, Compare>::operator[](key_type&& key) {
  auto iter = find(key);
  if (iter == end()) {
    return emplace(std::move(key), T()).first->second;
  }
  return iter->second;
}

template <class Key, class T, class Compare>
T& Map<Key, T, Compare>::operator[](const key_type& key) {
  auto iter = find(key);
  if (iter == end()) {
    return emplace(key, T()).first->second;
  }
  return iter->second;
}

template <class Key, class T, class Compare>
typename Map<Key, T, Compare>::iterator Map<Key, T, Compare>::find(
    const key_type& k) {
  Entry* iter = root_;
  while (iter != nullptr) {
    int comp = CompareKeys(iter->pair.first, k);
    if (comp == 0) {
      return iterator(this, iter);
    } else if (comp < 0) {
      iter = iter->right;
    } else {
      iter = iter->left;
    }
  }
  return end();
}

template <class Key, class T, class Compare>
template <class... Args>
typename ::grpc_core::Pair<typename Map<Key, T, Compare>::iterator, bool>
Map<Key, T, Compare>::emplace(Args&&... args) {
  Pair<key_type, mapped_type> pair(std::forward<Args>(args)...);
  iterator ret = find(pair.first);
  bool insertion = false;
  if (ret == end()) {
    Pair<iterator, Entry*> p = InsertRecursive(root_, std::move(pair));
    root_ = p.second;
    ret = p.first;
    insertion = true;
    size_++;
  }
  return MakePair(ret, insertion);
}

template <class Key, class T, class Compare>
size_t Map<Key, T, Compare>::erase(const key_type& key) {
  iterator it = find(key);
  if (it == end()) return 0;
  erase(it);
  return 1;
}

// TODO(mhaidry): Modify erase to use the iterator location
// to create an efficient erase method
template <class Key, class T, class Compare>
typename Map<Key, T, Compare>::iterator Map<Key, T, Compare>::erase(
    iterator iter) {
  if (iter == end()) return iter;
  key_type& del_key = iter->first;
  Pair<iterator, Entry*> ret = RemoveRecursive(root_, del_key);
  root_ = ret.second;
  size_--;
  return ret.first;
}

template <class Key, class T, class Compare>
typename Map<Key, T, Compare>::Entry* Map<Key, T, Compare>::InOrderSuccessor(
    const Entry* e) const {
  if (e->right != nullptr) {
    return GetMinEntry(e->right);
  }
  Entry* successor = nullptr;
  Entry* iter = root_;
  while (iter != nullptr) {
    int comp = CompareKeys(iter->pair.first, e->pair.first);
    if (comp > 0) {
      successor = iter;
      iter = iter->left;
    } else if (comp < 0) {
      iter = iter->right;
    } else
      break;
  }
  return successor;
}

// Returns a pair with the first value being an iterator pointing to the
// inserted entry and the second value being the new root of the subtree
// after a rebalance
template <class Key, class T, class Compare>
typename ::grpc_core::Pair<typename Map<Key, T, Compare>::iterator,
                           typename Map<Key, T, Compare>::Entry*>
Map<Key, T, Compare>::InsertRecursive(Entry* root, value_type&& p) {
  if (root == nullptr) {
    Entry* e = New<Entry>(std::move(p));
    return MakePair(iterator(this, e), e);
  }
  int comp = CompareKeys(root->pair.first, p.first);
  if (comp > 0) {
    Pair<iterator, Entry*> ret = InsertRecursive(root->left, std::move(p));
    root->left = ret.second;
    ret.second = RebalanceTreeAfterInsertion(root, ret.first->first);
    return ret;
  } else if (comp < 0) {
    Pair<iterator, Entry*> ret = InsertRecursive(root->right, std::move(p));
    root->right = ret.second;
    ret.second = RebalanceTreeAfterInsertion(root, ret.first->first);
    return ret;
  } else {
    root->pair = std::move(p);
    return MakePair(iterator(this, root), root);
  }
}

template <class Key, class T, class Compare>
typename Map<Key, T, Compare>::Entry* Map<Key, T, Compare>::GetMinEntry(
    Entry* e) {
  if (e != nullptr) {
    while (e->left != nullptr) {
      e = e->left;
    }
  }
  return e;
}

template <class Key, class T, class Compare>
typename Map<Key, T, Compare>::Entry* Map<Key, T, Compare>::RotateLeft(
    Entry* e) {
  Entry* rightChild = e->right;
  Entry* rightLeftChild = rightChild->left;
  rightChild->left = e;
  e->right = rightLeftChild;
  e->height = 1 + GPR_MAX(EntryHeight(e->left), EntryHeight(e->right));
  rightChild->height = 1 + GPR_MAX(EntryHeight(rightChild->left),
                                   EntryHeight(rightChild->right));
  return rightChild;
}

template <class Key, class T, class Compare>
typename Map<Key, T, Compare>::Entry* Map<Key, T, Compare>::RotateRight(
    Entry* e) {
  Entry* leftChild = e->left;
  Entry* leftRightChild = leftChild->right;
  leftChild->right = e;
  e->left = leftRightChild;
  e->height = 1 + GPR_MAX(EntryHeight(e->left), EntryHeight(e->right));
  leftChild->height =
      1 + GPR_MAX(EntryHeight(leftChild->left), EntryHeight(leftChild->right));
  return leftChild;
}

template <class Key, class T, class Compare>
typename Map<Key, T, Compare>::Entry*
Map<Key, T, Compare>::RebalanceTreeAfterInsertion(Entry* root,
                                                  const key_type& k) {
  root->height = 1 + GPR_MAX(EntryHeight(root->left), EntryHeight(root->right));
  int32_t heightDifference = EntryHeight(root->left) - EntryHeight(root->right);
  if (heightDifference > 1 && CompareKeys(root->left->pair.first, k) > 0) {
    return RotateRight(root);
  }
  if (heightDifference < -1 && CompareKeys(root->right->pair.first, k) < 0) {
    return RotateLeft(root);
  }
  if (heightDifference > 1 && CompareKeys(root->left->pair.first, k) < 0) {
    root->left = RotateLeft(root->left);
    return RotateRight(root);
  }
  if (heightDifference < -1 && CompareKeys(root->right->pair.first, k) > 0) {
    root->right = RotateRight(root->right);
    return RotateLeft(root);
  }
  return root;
}

template <class Key, class T, class Compare>
typename Map<Key, T, Compare>::Entry*
Map<Key, T, Compare>::RebalanceTreeAfterDeletion(Entry* root) {
  root->height = 1 + GPR_MAX(EntryHeight(root->left), EntryHeight(root->right));
  int32_t heightDifference = EntryHeight(root->left) - EntryHeight(root->right);
  if (heightDifference > 1) {
    int leftHeightDifference =
        EntryHeight(root->left->left) - EntryHeight(root->left->right);
    if (leftHeightDifference < 0) {
      root->left = RotateLeft(root->left);
    }
    return RotateRight(root);
  }
  if (heightDifference < -1) {
    int rightHeightDifference =
        EntryHeight(root->right->left) - EntryHeight(root->right->right);
    if (rightHeightDifference > 0) {
      root->right = RotateRight(root->right);
    }
    return RotateLeft(root);
  }
  return root;
}

template <class Key, class T, class Compare>
typename ::grpc_core::Pair<typename Map<Key, T, Compare>::iterator,
                           typename Map<Key, T, Compare>::Entry*>
Map<Key, T, Compare>::RemoveRecursive(Entry* root, const key_type& k) {
  Pair<iterator, Entry*> ret = MakePair(end(), root);
  if (root == nullptr) return ret;
  int comp = CompareKeys(root->pair.first, k);
  if (comp > 0) {
    ret = RemoveRecursive(root->left, k);
    root->left = ret.second;
  } else if (comp < 0) {
    ret = RemoveRecursive(root->right, k);
    root->right = ret.second;
  } else {
    Entry* entry;
    Entry* successor = InOrderSuccessor(root);
    if (root->left == nullptr) {
      entry = root->right;
      Delete(root);
      return MakePair(iterator(this, successor), entry);
    } else if (root->right == nullptr) {
      entry = root->left;
      Delete(root);
      return MakePair(iterator(this, successor), entry);
    } else {
      entry = successor;
      root->pair.swap(entry->pair);
      ret = RemoveRecursive(root->right, entry->pair.first);
      root->right = ret.second;
      ret.first = iterator(this, root);
    }
  }
  return MakePair(ret.first, RebalanceTreeAfterDeletion(root));
}

template <class Key, class T, class Compare>
int Map<Key, T, Compare>::CompareKeys(const key_type& lhs,
                                      const key_type& rhs) {
  // This is a workaround for "const key_compare compare;"
  // because some versions of compilers cannot build this by requiring
  // a user-provided constructor. (ref: https://stackoverflow.com/q/7411515)
  key_compare compare_tmp;
  const key_compare& compare = compare_tmp;
  bool left_comparison = compare(lhs, rhs);
  bool right_comparison = compare(rhs, lhs);
  // Both values are equal
  if (!left_comparison && !right_comparison) {
    return 0;
  }
  return left_comparison ? -1 : 1;
}
}  // namespace grpc_core
#endif /* GRPC_CORE_LIB_GPRPP_MAP_H */
