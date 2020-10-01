/****************************************************************************
 * Copyright (c) 2012-2020 by the ArborX authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the ArborX library. ArborX is                       *
 * distributed under a BSD 3-clause license. For the licensing terms see    *
 * the LICENSE file in the top-level directory.                             *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/
#ifndef ARBORX_DETAILS_TREE_TRAVERSAL_HPP
#define ARBORX_DETAILS_TREE_TRAVERSAL_HPP

#include <ArborX_AccessTraits.hpp>
#include <ArborX_DetailsAlgorithms.hpp>
#include <ArborX_DetailsNode.hpp> // ROPE_SENTINEL
#include <ArborX_DetailsPriorityQueue.hpp>
#include <ArborX_DetailsStack.hpp>
#include <ArborX_DetailsUtils.hpp>
#include <ArborX_Exception.hpp>
#include <ArborX_Predicates.hpp>

namespace ArborX
{
namespace Details
{

template <typename BVH, typename Predicates, typename Callback, typename Tag>
struct TreeTraversal
{
};

template <typename BVH, typename Predicates, typename Callback>
struct TreeTraversal<BVH, Predicates, Callback, SpatialPredicateTag>
{
  BVH bvh_;
  Predicates predicates_;
  Callback callback_;

  using Access = AccessTraits<Predicates, PredicatesTag>;
  using Node = typename BVH::node_type;

  template <typename ExecutionSpace>
  TreeTraversal(ExecutionSpace const &space, BVH const &bvh,
                Predicates const &predicates, Callback const &callback)
      : bvh_{bvh}
      , predicates_{predicates}
      , callback_{callback}
  {
    if (bvh_.empty())
    {
      // do nothing
    }
    else if (bvh_.size() == 1)
    {
      Kokkos::parallel_for(
          "ArborX::TreeTraversal::spatial::degenerated_one_leaf_tree",
          Kokkos::RangePolicy<ExecutionSpace, OneLeafTree>(
              space, 0, Access::size(predicates)),
          *this);
    }
    else
    {
      static_assert(
          std::is_same<typename Node::Tag, NodeWithTwoChildrenTag>{} ||
              std::is_same<typename Node::Tag, NodeWithLeftChildAndRopeTag>{},
          "Unrecognized node tag");

      Kokkos::parallel_for("ArborX::TreeTraversal::spatial",
                           Kokkos::RangePolicy<ExecutionSpace>(
                               space, 0, Access::size(predicates)),
                           *this);
    }
  }

  struct OneLeafTree
  {
  };

  KOKKOS_FUNCTION void operator()(OneLeafTree, int queryIndex) const
  {
    auto const &predicate = Access::get(predicates_, queryIndex);

    if (predicate(bvh_.getBoundingVolume(bvh_.getRoot())))
    {
      callback_(predicate, 0);
    }
  }

  // Stack-based traversal
  template <typename Tag = typename Node::Tag>
  KOKKOS_FUNCTION std::enable_if_t<std::is_same<Tag, NodeWithTwoChildrenTag>{}>
  operator()(int queryIndex) const
  {
    auto const &predicate = Access::get(predicates_, queryIndex);

    Node const *stack[64];
    Node const **stack_ptr = stack;
    *stack_ptr++ = nullptr;
    Node const *node = bvh_.getRoot();
    do
    {
      Node const *child_left = bvh_.getNodePtr(node->left_child);
      Node const *child_right = bvh_.getNodePtr(node->right_child);

      bool overlap_left = predicate(bvh_.getBoundingVolume(child_left));
      bool overlap_right = predicate(bvh_.getBoundingVolume(child_right));

      if (overlap_left && child_left->isLeaf())
      {
        callback_(predicate, child_left->getLeafPermutationIndex());
      }
      if (overlap_right && child_right->isLeaf())
      {
        callback_(predicate, child_right->getLeafPermutationIndex());
      }

      bool traverse_left = (overlap_left && !child_left->isLeaf());
      bool traverse_right = (overlap_right && !child_right->isLeaf());

      if (!traverse_left && !traverse_right)
      {
        node = *--stack_ptr;
      }
      else
      {
        node = traverse_left ? child_left : child_right;
        if (traverse_left && traverse_right)
          *stack_ptr++ = child_right;
      }
    } while (node != nullptr);
  }

  // Ropes-based traversal
  template <typename Tag = typename Node::Tag>
  KOKKOS_FUNCTION
      std::enable_if_t<std::is_same<Tag, NodeWithLeftChildAndRopeTag>{}>
      operator()(int queryIndex) const
  {
    auto const &predicate = Access::get(predicates_, queryIndex);

    Node const *node;
    int next = 0; // start with root
    do
    {
      node = bvh_.getNodePtr(next);

      if (predicate(bvh_.getBoundingVolume(node)))
      {
        if (!node->isLeaf())
        {
          next = node->left_child;
        }
        else
        {
          callback_(predicate, node->getLeafPermutationIndex());
          next = node->rope;
        }
      }
      else
      {
        next = node->rope;
      }

    } while (next != ROPE_SENTINEL);
  }
};

template <typename BVH, typename Predicates, typename Callback>
struct TreeTraversal<BVH, Predicates, Callback, NearestPredicateTag>
{
  using MemorySpace = typename BVH::memory_space;

  BVH bvh_;
  Predicates predicates_;
  Callback callback_;

  using Access = AccessTraits<Predicates, PredicatesTag>;
  using Node = typename BVH::node_type;

  using Buffer = Kokkos::View<Kokkos::pair<int, float> *, MemorySpace>;
  using Offset = Kokkos::View<int *, MemorySpace>;
  struct BufferProvider
  {
    Buffer buffer_;
    Offset offset_;

    KOKKOS_FUNCTION auto operator()(int i) const
    {
      auto const *offset_ptr = &offset_(i);
      return Kokkos::subview(buffer_,
                             Kokkos::make_pair(*offset_ptr, *(offset_ptr + 1)));
    }
  };

  BufferProvider buffer_;

  template <typename ExecutionSpace>
  void allocateBuffer(ExecutionSpace const &space)
  {
    auto const n_queries = Access::size(predicates_);

    Offset offset(Kokkos::ViewAllocateWithoutInitializing(
                      "ArborX::TreeTraversal::nearest::offset"),
                  n_queries + 1);
    // NOTE workaround to avoid implicit capture of *this
    auto const &predicates = predicates_;
    Kokkos::parallel_for(
        "ArborX::TreeTraversal::nearest::"
        "scan_queries_for_numbers_of_neighbors",
        Kokkos::RangePolicy<ExecutionSpace>(space, 0, n_queries),
        KOKKOS_LAMBDA(int i) { offset(i) = getK(Access::get(predicates, i)); });
    exclusivePrefixSum(space, offset);
    int const buffer_size = lastElement(offset);
    // Allocate buffer over which to perform heap operations in
    // TreeTraversal::nearestQuery() to store nearest leaf nodes found so far.
    // It is not possible to anticipate how much memory to allocate since the
    // number of nearest neighbors k is only known at runtime.

    Buffer buffer(Kokkos::ViewAllocateWithoutInitializing(
                      "ArborX::TreeTraversal::nearest::buffer"),
                  buffer_size);
    buffer_ = BufferProvider{buffer, offset};
  }

  template <typename ExecutionSpace>
  TreeTraversal(ExecutionSpace const &space, BVH const &bvh,
                Predicates const &predicates, Callback const &callback)
      : bvh_{bvh}
      , predicates_{predicates}
      , callback_{callback}
  {
    if (bvh_.empty())
    {
      // do nothing
    }
    else if (bvh_.size() == 1)
    {
      Kokkos::parallel_for(
          "ArborX::TreeTraversal::nearest::degenerated_one_leaf_tree",
          Kokkos::RangePolicy<ExecutionSpace, OneLeafTree>(
              space, 0, Access::size(predicates)),
          *this);
    }
    else
    {
      static_assert(
          std::is_same<typename Node::Tag, NodeWithLeftChildAndRopeTag>{} ||
              std::is_same<typename Node::Tag, NodeWithTwoChildrenTag>{},
          "Unrecognized node tag");

      allocateBuffer(space);

      Kokkos::parallel_for("ArborX::TreeTraversal::nearest",
                           Kokkos::RangePolicy<ExecutionSpace>(
                               space, 0, Access::size(predicates)),
                           *this);
    }
  }

  struct OneLeafTree
  {
  };

  KOKKOS_FUNCTION int operator()(OneLeafTree, int queryIndex) const
  {
    auto const &predicate = Access::get(predicates_, queryIndex);
    auto const k = getK(predicate);
    auto const distance = [geometry = getGeometry(predicate),
                           bvh = bvh_](Node const *node) {
      return Details::distance(geometry, bvh.getBoundingVolume(node));
    };

    // NOTE thinking about making this a precondition
    if (k < 1)
      return 0;

    callback_(predicate, 0, distance(bvh_.getRoot()));
    return 1;
  }

  template <typename Tag = typename Node::Tag>
  KOKKOS_FUNCTION
      std::enable_if_t<std::is_same<Tag, NodeWithTwoChildrenTag>{}, int>
      getRightChild(Node const *node) const
  {
    return node->right_child;
  }

  template <typename Tag = typename Node::Tag>
  KOKKOS_FUNCTION
      std::enable_if_t<std::is_same<Tag, NodeWithLeftChildAndRopeTag>{}, int>
      getRightChild(Node const *node) const
  {
    assert(!node->isLeaf());
    return bvh_.getNodePtr(node->left_child)->rope;
  }

  KOKKOS_FUNCTION int operator()(int queryIndex) const
  {
    auto const &predicate = Access::get(predicates_, queryIndex);
    auto const k = getK(predicate);
    auto const distance = [geometry = getGeometry(predicate),
                           bvh = bvh_](Node const *node) {
      return Details::distance(geometry, bvh.getBoundingVolume(node));
    };
    auto const buffer = buffer_(queryIndex);

    // NOTE thinking about making this a precondition
    if (k < 1)
      return 0;

    // Nodes with a distance that exceed that radius can safely be
    // discarded. Initialize the radius to infinity and tighten it once k
    // neighbors have been found.
    auto radius = KokkosExt::ArithmeticTraits::infinity<float>::value;

    using PairIndexDistance = Kokkos::pair<int, float>;
    static_assert(
        std::is_same<typename decltype(buffer)::value_type,
                     PairIndexDistance>::value,
        "Type of the elements stored in the buffer passed as argument to "
        "TreeTraversal::nearestQuery is not right");
    struct CompareDistance
    {
      KOKKOS_INLINE_FUNCTION bool operator()(PairIndexDistance const &lhs,
                                             PairIndexDistance const &rhs) const
      {
        return lhs.second < rhs.second;
      }
    };
    // Use a priority queue for convenience to store the results and
    // preserve the heap structure internally at all time.  There is no
    // memory allocation, elements are stored in the buffer passed as an
    // argument. The farthest leaf node is on top.
    assert(k == (int)buffer.size());
    PriorityQueue<PairIndexDistance, CompareDistance,
                  UnmanagedStaticVector<PairIndexDistance>>
        heap(UnmanagedStaticVector<PairIndexDistance>(buffer.data(),
                                                      buffer.size()));

    Node const *stack[64];
    auto *stack_ptr = stack;
    *stack_ptr++ = nullptr;
#if !defined(__CUDA_ARCH__)
    float stack_distance[64];
    auto *stack_distance_ptr = stack_distance;
    *stack_distance_ptr++ = 0.f;
#endif

    Node const *node = bvh_.getRoot();
    Node const *child_left = nullptr;
    Node const *child_right = nullptr;

    float distance_left = 0.f;
    float distance_right = 0.f;
    float distance_node = 0.f;

    do
    {
      bool traverse_left = false;
      bool traverse_right = false;

      if (distance_node < radius)
      {
        // Insert children into the stack and make sure that the
        // closest one ends on top.
        child_left = bvh_.getNodePtr(node->left_child);
        child_right = bvh_.getNodePtr(getRightChild(node));

        distance_left = distance(child_left);
        distance_right = distance(child_right);

        if (distance_left < radius && child_left->isLeaf())
        {
          auto leaf_pair = Kokkos::make_pair(
              child_left->getLeafPermutationIndex(), distance_left);
          if ((int)heap.size() < k)
            heap.push(leaf_pair);
          else
            heap.popPush(leaf_pair);
          if ((int)heap.size() == k)
            radius = heap.top().second;
        }

        // Note: radius may have been already updated here from the left child
        if (distance_right < radius && child_right->isLeaf())
        {
          auto leaf_pair = Kokkos::make_pair(
              child_right->getLeafPermutationIndex(), distance_right);
          if ((int)heap.size() < k)
            heap.push(leaf_pair);
          else
            heap.popPush(leaf_pair);
          if ((int)heap.size() == k)
            radius = heap.top().second;
        }

        traverse_left = (distance_left < radius && !child_left->isLeaf());
        traverse_right = (distance_right < radius && !child_right->isLeaf());
      }

      if (!traverse_left && !traverse_right)
      {
        node = *--stack_ptr;
#if defined(__CUDA_ARCH__)
        if (node != nullptr)
        {
          // This is a theoretically unnecessary duplication of distance
          // calculation for stack nodes. However, for Cuda it's better than
          // than putting the distances in stack.
          distance_node = distance(node);
        }
#else
        distance_node = *--stack_distance_ptr;
#endif
      }
      else
      {
        node = (traverse_left &&
                (distance_left <= distance_right || !traverse_right))
                   ? child_left
                   : child_right;
        distance_node = (node == child_left ? distance_left : distance_right);
        if (traverse_left && traverse_right)
        {
          *stack_ptr++ = (node == child_left ? child_right : child_left);
#if !defined(__CUDA_ARCH__)
          *stack_distance_ptr++ =
              (node == child_left ? distance_right : distance_left);
#endif
        }
      }
    } while (node != nullptr);

    // Sort the leaf nodes and output the results.
    // NOTE: Do not try this at home.  Messing with the underlying container
    // invalidates the state of the PriorityQueue.
    sortHeap(heap.data(), heap.data() + heap.size(), heap.valueComp());
    for (decltype(heap.size()) i = 0; i < heap.size(); ++i)
    {
      int const leaf_index = (heap.data() + i)->first;
      auto const leaf_distance = (heap.data() + i)->second;
      callback_(predicate, leaf_index, leaf_distance);
    }
    return heap.size();
  }
};

template <typename ExecutionSpace, typename BVH, typename Predicates,
          typename Callback>
void traverse(ExecutionSpace const &space, BVH const &bvh,
              Predicates const &predicates, Callback const &callback)
{
  using Access = AccessTraits<Predicates, PredicatesTag>;
  using Tag = typename AccessTraitsHelper<Access>::tag;
  TreeTraversal<BVH, Predicates, Callback, Tag>(space, bvh, predicates,
                                                callback);
}

} // namespace Details
} // namespace ArborX

#endif
