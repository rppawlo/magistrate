
#if !defined INCLUDED_SERDES_CONTAINER_VIEW_SERIALIZE_H
#define INCLUDED_SERDES_CONTAINER_VIEW_SERIALIZE_H

#include "serdes_common.h"
#include "serializers/serializers_headers.h"

#if KOKKOS_ENABLED_SERDES

#include <Kokkos_Core.hpp>
#include <Kokkos_View.hpp>
#include <Kokkos_DynamicView.hpp>
#include <Kokkos_Serial.hpp>

#include <array>
#include <cassert>
#include <tuple>
#include <type_traits>
#include <utility>

#define CHECKPOINT_KOKKOS_PACK_EXPLICIT_EXTENTS 0

namespace serdes {

namespace detail {

template <typename SerializerT, typename ViewType, typename T>
struct Helper {
  static constexpr size_t dynamic_count = 0;
  static int countRTDim(ViewType const& view) { return 0; }
};

template <typename SerializerT, typename ViewType, typename T>
struct Helper<SerializerT, ViewType, T*> {
  static constexpr size_t dynamic_count =
    Helper<SerializerT, ViewType, T>::dynamic_count + 1;

  static int countRTDim(ViewType const& view) {
    auto const val = Helper<SerializerT, ViewType, T>::countRTDim(view);
    return val + 1;
  }
};

template <typename SerializerT, typename ViewType, typename T, size_t N>
struct Helper<SerializerT, ViewType, T[N]> {
  static constexpr size_t dynamic_count =
    Helper<SerializerT, ViewType, T>::dynamic_count;
  static int countRTDim(ViewType const& view) {
    auto const val = Helper<SerializerT, ViewType, T>::countRTDim(view);
    return val + 1;
  }
};
} // namespace detail


template <typename SerializerT, typename ViewType, std::size_t dims>
struct TraverseManual;

template <typename SerializerT, typename ViewType>
struct TraverseManual<SerializerT,ViewType,1> {
  static void apply(SerializerT& s, ViewType const& v) {
    for (typename ViewType::size_type i = 0; i < v.extent(0); i++) {
      s | v.operator()(i);
    }
  }
};

template <typename SerializerT, typename ViewType>
struct TraverseManual<SerializerT,ViewType,2> {
  static void apply(SerializerT& s, ViewType const& v) {
    for (typename ViewType::size_type i = 0; i < v.extent(0); i++) {
      for (typename ViewType::size_type j = 0; j < v.extent(1); j++) {
        s | v.operator()(i,j);
      }
    }
  }
};

template <typename SerializerT, typename ViewType>
struct TraverseManual<SerializerT,ViewType,3> {
  static void apply(SerializerT& s, ViewType const& v) {
    for (typename ViewType::size_type i = 0; i < v.extent(0); i++) {
      for (typename ViewType::size_type j = 0; j < v.extent(1); j++) {
        for (typename ViewType::size_type k = 0; k < v.extent(2); k++) {
          s | v.operator()(i,j,k);
        }
      }
    }
  }
};

template <typename SerializerT, typename ViewType>
struct TraverseManual<SerializerT,ViewType,4> {
  static void apply(SerializerT& s, ViewType const& v) {
    for (typename ViewType::size_type i = 0; i < v.extent(0); i++) {
      for (typename ViewType::size_type j = 0; j < v.extent(1); j++) {
        for (typename ViewType::size_type k = 0; k < v.extent(2); k++) {
          for (typename ViewType::size_type l = 0; l < v.extent(3); l++) {
            s | v.operator()(i,j,k,l);
          }
        }
      }
    }
  }
};

template <typename ViewType, std::size_t N,typename... I>
static ViewType buildView(
  std::string const& label, typename ViewType::pointer_type const val_ptr,
  I&&... index
) {
  ViewType v{label, std::forward<I>(index)...};
  return v;
}

template <typename ViewType, typename Tuple, std::size_t... I>
static constexpr ViewType apply_impl(
  std::string const& view_label, typename ViewType::pointer_type const val_ptr,
  Tuple&& t, std::index_sequence<I...>
) {
  constexpr auto const rank_val = ViewType::Rank;
  return buildView<ViewType,rank_val>(
    view_label,val_ptr,std::get<I>(std::forward<Tuple>(t))...
  );
}

template <typename ViewType, typename Tuple>
static constexpr ViewType apply(
  std::string const& view_label, typename ViewType::pointer_type const val_ptr,
  Tuple&& t
) {
  using TupUnrefT = std::remove_reference_t<Tuple>;
  constexpr auto tup_size = std::tuple_size<TupUnrefT>::value;
  return apply_impl<ViewType>(
    view_label, val_ptr, std::forward<Tuple>(t),
    std::make_index_sequence<tup_size>{}
  );
}

template <typename SerdesT>
inline void serializeLayout(SerdesT& s, int dim, Kokkos::LayoutStride& layout) {
  for (auto i = 0; i < dim; i++) {
    s | layout.dimension[i];
    s | layout.stride[i];
  }
}

template <typename SerdesT>
inline void serializeLayout(SerdesT& s, int dim, Kokkos::LayoutLeft& layout) {
  for (auto i = 0; i < dim; i++) {
    s | layout.dimension[i];
  }
}

template <typename SerdesT>
inline void serializeLayout(SerdesT& s, int dim, Kokkos::LayoutRight& layout) {
  for (auto i = 0; i < dim; i++) {
    s | layout.dimension[i];
  }
}

// template <typename... Args>
// using DynamicViewType = Kokkos::Experimental::DynamicView<Args...>;

template <typename SerializerT, typename ViewT>
inline std::string serializeViewLabel(SerializerT& s, ViewT& view) {
  static constexpr auto const is_managed = ViewT::traits::is_managed;

  // Serialize the label of the view
  std::string view_label = view.label();
  if (is_managed) {
    s | view_label;
  } else {
    assert(0 && "Unmanaged not handled currently");
  }

  return view_label;
}

template <typename SerializerT, typename T, typename... Args>
inline void serialize(
  SerializerT& s, Kokkos::Experimental::DynamicView<T,Args...>& view
) {
  using ViewType = Kokkos::Experimental::DynamicView<T,Args...>;

  // serialize the label
  auto const label = serializeViewLabel(s,view);

  std::cout << "label:" << label << std::endl;

  // serialize the min chunk size and extent which is used to recreate this
  std::size_t chunk_size = 0;
  std::size_t max_extent = 0;
  std::size_t view_size = 0;

  if (!s.isUnpacking()) {
    chunk_size = view.chunk_size();
    max_extent = view.allocation_extent();
    view_size = view.size();
  }

  s | chunk_size;
  s | max_extent;
  s | view_size;

  // std::cout << "chunk_size:" << chunk_size << std::endl;
  // std::cout << "max_extent:" << max_extent << std::endl;
  // std::cout << "view_size:" << view_size << std::endl;

  if (s.isUnpacking()) {
    unsigned min_chunk_size = static_cast<unsigned>(chunk_size);
    unsigned max_alloc_extent = static_cast<unsigned>(max_extent);
    view = apply<ViewType>(
      label, nullptr, std::make_tuple(min_chunk_size,max_alloc_extent)
    );
    view.resize_serial(view_size);
  }

  TraverseManual<SerializerT,ViewType,1>::apply(s,view);
}

template <typename SerializerT, typename T, typename... Args>
inline void serialize(SerializerT& s, Kokkos::View<T,Args...>& view) {
  using ViewType = Kokkos::View<T,Args...>;
  using ViewTraitsType = Kokkos::ViewTraits<T,Args...>;

  static constexpr auto const rank_val = ViewType::Rank;
  static constexpr auto const is_managed = ViewType::traits::is_managed;

  // Serialize the label of the view
  std::string view_label = view.label();
  if (is_managed) {
    s | view_label;
  } else {
    assert(0 && "Unmanaged not handled currently");
  }

  int rt_dim = 0;
  if (!s.isUnpacking()) {
    rt_dim = detail::Helper<SerializerT, ViewType, T>::countRTDim(view);
  }
  s | rt_dim;

  // std::cout << "rt_dim: "<< rt_dim <<std::endl;

  // This is explicitly done like this because the view.layout() might fail
  // before proper initialization
  typename ViewType::traits::array_layout layout;

  if (s.isUnpacking()) {
    serializeLayout<SerializerT>(s, rt_dim, layout);
    // do something with the layout now
  } else {
    typename ViewType::traits::array_layout layout_cur = view.layout();
    serializeLayout<SerializerT>(s, rt_dim, layout_cur);
  }

#if CHECKPOINT_KOKKOS_PACK_EXPLICIT_EXTENTS
  constexpr auto dyn_dims =
    detail::Helper<SerializerT, ViewType, T>::dynamic_count;

  std::array<size_t, dyn_dims> dynamicExtentsArray;

  if (s.isUnpacking()) {
    s | dynamicExtentsArray;
  } else {
    for (auto i = 0; i < dyn_dims; i++) {
      dynamicExtentsArray[i] = view.extent(i);
    }
    s | dynamicExtentsArray;
  }
#endif

  if (is_managed) {
    size_t num_elms = view.size();
    s | num_elms;

    if (s.isUnpacking()) {
      view = apply<ViewType>(view_label, nullptr, std::make_tuple(layout));
    }

    bool is_contig = view.span_is_contiguous();
    s | is_contig;

    if (is_contig) {
      // Serialize the data out of the buffer directly into the internal
      // allocated memory
      serializeArray(s, view.data(), num_elms);
    } else {
      TraverseManual<SerializerT,ViewType,rank_val>::apply(s,view);
    }
  } else {
    assert(0 && "Unmanaged not handled currently");
  }
}

} /* end namespace serdes */

#endif /*KOKKOS_ENABLED_SERDES*/

#endif /*INCLUDED_SERDES_CONTAINER_VIEW_SERIALIZE_H*/
