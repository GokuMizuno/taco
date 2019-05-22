#ifndef TACO_TENSOR_H
#define TACO_TENSOR_H

#include <memory>
#include <string>
#include <vector>
#include <cassert>
#include <utility>
#include <array>

#include "taco/type.h"
#include "taco/format.h"

#include "taco/codegen/module.h"

#include "taco/index_notation/index_notation.h"

#include "taco/storage/storage.h"
#include "taco/storage/index.h"
#include "taco/storage/array.h"
#include "taco/storage/typed_vector.h"
#include "taco/storage/typed_index.h"

#include "taco/error.h"
#include "taco/error/error_messages.h"
#include "taco/util/name_generator.h"
#include "taco/util/strings.h"


namespace taco {

/// TensorBase is the super-class for all tensors. You can use it directly to
/// avoid templates, or you can use the templated `Tensor<T>` that inherits from
/// `TensorBase`.
class TensorBase {
public:
  /* --- Constructor Methods --- */

  /// Create a scalar
  TensorBase();

  /// Create a scalar
  TensorBase(Datatype ctype);

  /// Create a scalar with the given name
  TensorBase(std::string name, Datatype ctype);

  /// Create a scalar
  template <typename CType>
  explicit TensorBase(CType val);
  
  /// Create a tensor with the given dimensions. The format defaults to sparse 
  /// in every mode.
  TensorBase(Datatype ctype, std::vector<int> dimensions, 
             ModeFormat modeType = ModeFormat::compressed);
  
  /// Create a tensor with the given dimensions and format.
  TensorBase(Datatype ctype, std::vector<int> dimensions, Format format); 

  /// Create a tensor with the given data type, dimensions and format. The 
  /// format defaults to sparse in every mode.
  TensorBase(std::string name, Datatype ctype, std::vector<int> dimensions, 
             ModeFormat modeType = ModeFormat::compressed); 
  
  /// Create a tensor with the given data type, dimensions and format.
  TensorBase(std::string name, Datatype ctype, std::vector<int> dimensions,
             Format format);

  /* --- Metadata Methods    --- */

  /// Set the name of the tensor.
  void setName(std::string name) const;

  /// Get the name of the tensor.
  std::string getName() const;

  /// Get the order of the tensor (the number of modes).
  int getOrder() const;

  /// Get the dimension of a tensor mode.
  int getDimension(int mode) const;

  /// Get a vector with the dimension of each tensor mode.
  const std::vector<int>& getDimensions() const;

  /// Return the type of the tensor components).
  const Datatype& getComponentType() const; 

  /// Get the format the tensor is packed into
  const Format& getFormat() const;

  /// Set the tensor's storage
  void setStorage(TensorStorage storage);

  /// Returns the storage for this tensor. Tensor values are stored according
  /// to the format of the tensor.
  const TensorStorage& getStorage() const;

  /// Returns the storage for this tensor. Tensor values are stored according
  /// to the format of the tensor.
  TensorStorage& getStorage();

  /// Returns the tensor var for this tensor.
  const TensorVar& getTensorVar() const;

  template<typename T, typename CType>
  class const_iterator {
  public:
    class Coordinates {
    public:
      const T& operator[](size_t idx) const {
        return coordinates[idx];
      }

      size_t getOrder() const {
        return order;
      }
    
      std::vector<T> toVector() const {
        std::vector<T> ret(order);
        for (size_t i = 0; i < order; ++i) {
          ret[i] = coordinates[i];
        }
        return ret;
      }

      friend bool operator==(const Coordinates& a, const Coordinates& b) {
        for (size_t i = 0; i < a.order; ++i) {
          if (a[i] != b[i]) return false;
        }
        return true;
      }

      friend bool operator!=(const Coordinates& a, const Coordinates& b) {
        return !(a == b);
      }
      
      friend bool operator<(const Coordinates& a, const Coordinates& b) {
        for (size_t i = 0; i < a.order; i++) {
          if (a[i] < b[i]) return true;
          if (a[i] > b[i]) return false;
        }
        return false;
      }

      friend std::ostream& operator<<(std::ostream& os, const Coordinates& c) {
        return os << util::join(c.toVector());
      }
    
    private:
      friend class const_iterator;

      Coordinates(size_t order) : 
          coordinates(nullptr), order(order) {
      }

      const T*     coordinates;
      const size_t order;
    };

    typedef const_iterator self_type;
    typedef std::pair<Coordinates,CType>  value_type;
    typedef std::pair<Coordinates,CType>& reference;
    typedef std::pair<Coordinates,CType>* pointer;
    typedef std::input_iterator_tag iterator_category;

    const_iterator& operator++() {
      ++bufferPos;
      curVal.first.coordinates += tensorOrder;

      // If iterator has iterated over every element in the buffer, refill the 
      // buffer with additional tensor elements.
      if (bufferPos >= bufferSize) {
        fillBuffer();
        bufferPos = 0;
        curVal.first.coordinates = ctx->coordBuffer;
        ++chunksIterated;
      }

      curVal.second = valBuffer[bufferPos];
      return *this;
    }

    const_iterator operator++(int) {
     const_iterator result = *this;
     ++(*this);
     return result;
    }

    const value_type& operator*() const {
      return curVal;
    }

    const value_type* operator->() const {
      return &curVal;
    }

    bool operator==(const const_iterator& rhs) {
      // Check that both iterators have reached the end or point to the same 
      // element and that both iterators iterate over the same tensor.
      return ((bufferSize == 0) == (rhs.bufferSize == 0)) && 
             ((bufferSize == 0) || (valsIterated() == rhs.valsIterated())) && 
             (tensor == rhs.tensor);
    }

    bool operator!=(const const_iterator& rhs) {
      return !(*this == rhs);
    }

  protected:
    int64_t valsIterated() const {
      return chunksIterated * bufferCapacity + bufferPos;
    }

  private:
    friend class TensorBase;

    struct Context {
      Context(int order, int bufferCapacity, void* iterCtx) :
          coordBuffer(new T[order * bufferCapacity]),
          valBuffer(new CType[bufferCapacity]),
          iterCtx(iterCtx) {
      }

      ~Context() {
        delete[] coordBuffer;
        delete[] valBuffer;
        free(iterCtx);
      }

      T* coordBuffer;
      CType* valBuffer;
      void* iterCtx;
    };

    const_iterator(const TensorBase* tensor, bool isEnd = false) :
        tensor(tensor),
        tensorStorage(tensor->getStorage()),
        tensorOrder(tensor->getOrder()),
        bufferCapacity(100),
        bufferSize(0),
        bufferPos(bufferSize),
        chunksIterated(-1),
        ctx(isEnd ? nullptr : makeContext(tensorOrder, bufferCapacity)), 
        valBuffer(ctx ? ctx->valBuffer : nullptr),
        curVal(Coordinates(tensorOrder), (CType)0) {
      if (!isEnd) {
        const auto helperFuncs = tensor->getHelperFunctions(tensor->getFormat(), 
            tensor->getComponentType(), tensor->getDimensions());
        *reinterpret_cast<void**>(&iterFunc) = 
            helperFuncs->getFuncPtr("_shim_iterate");
        ++(*this);
      }
    }

    static std::shared_ptr<Context> makeContext(int tensorOrder, 
                                                int bufferCapacity) {
      return std::make_shared<Context>(tensorOrder, bufferCapacity, nullptr);
    }

    void fillBuffer() {
      std::array<void*,5> args = {&ctx->iterCtx, ctx->coordBuffer, 
                                  (void*)valBuffer, (void*)&bufferCapacity, 
                                  (void*)tensorStorage};
      bufferSize = iterFunc(args.data());
    }

    typedef int (*fnptr_t)(void**);

    const TensorBase*              tensor;
    const taco_tensor_t*           tensorStorage;
    const int                      tensorOrder;
    const int                      bufferCapacity;
    int                            bufferSize;
    int                            bufferPos;
    int64_t                        chunksIterated;
    fnptr_t                        iterFunc;
    const std::shared_ptr<Context> ctx;
    const CType*                   valBuffer;
    value_type                     curVal;
  };

  /// Wrapper to template the index and value types used during
  /// value iteration for performance.
  template<typename T, typename CType>
  class iterator_wrapper {
  public:
    const_iterator<T, CType> begin() const {
      return const_iterator<T, CType>(tensor);
    }

    const_iterator<T, CType> end() const {
      return const_iterator<T, CType>(tensor, true);
    }

  private:
    friend class TensorBase;

    iterator_wrapper(const TensorBase* tensor) : tensor(tensor) { }

    const TensorBase* tensor;
  };

  /// Get an object that can be used to instantiate a foreach loop
  /// to iterate over the values in the storage object.
  /// CType: type of the values stored. Must match the component type
  ///        for correct behavior.
  /// Example usage:
  /// for (auto& component : tensor.iterator<double>()) { ... }
  template<typename CType>
  iterator_wrapper<int,CType> iterator() const {
    return TensorBase::iterator_wrapper<int,CType>(this);
  }

  template<typename T, typename CType>
  iterator_wrapper<T,CType> iteratorTyped() const {
    return TensorBase::iterator_wrapper<T,CType>(this);
  }

  /// Set the expression to be evaluated when calling compute or assemble.
  void setAssignment(Assignment assignment);

  /// Set the expression to be evaluated when calling compute or assemble.
  Assignment getAssignment() const;

  /// Reserve space for `numCoordinates` additional coordinates.
  void reserve(size_t numCoordinates);

  /* --- Write Methods       --- */

  /// Insert a value into the tensor. The number of coordinates must match the
  /// tensor order.
  template <typename CType>
  void insert(const std::initializer_list<int>& coordinate, CType value);

  /// Insert a value into the tensor. The number of coordinates must match the
  /// tensor order.
  template <typename CType>
  void insert(const std::vector<int>& coordinate, CType value);

  /// Fill the tensor with the list of components defined by the iterator range (begin, end).
  ///
  /// The input list of triplets does not have to be sorted, and can contains duplicated elements.
  /// The result is a Tensor where the duplicates have been summed up.
  /// The InputIterators value_type must provide the following interface:
  ///
  /// CType value() const;                    // the value
  /// Coordinate<order> dimensions() const;   // the coordinate
  /// 
  /// See for instance the taco::Component template class.
  template <typename InputIterators>
  void setFromComponents(const InputIterators& begin, const InputIterators& end);

  /// The same as setFromComponents but when duplicates are met the functor dup_func is applied:
  ///
  /// value = dup_func(OldValue, NewValue)
  template <typename InputIterators, typename DupFunctor>
  void setFromComponents(const InputIterators& begin, const InputIterators& end, DupFunctor dup_func);

  /* --- Read Methods        --- */

  template <typename CType>  
  CType at(const std::vector<int>& coordinate);

  /* --- Access Methods      --- */

  /// Create an index expression that accesses (reads) this tensor.
  const Access operator()(const std::vector<IndexVar>& indices) const;

  /// Create an index expression that accesses (reads or writes) this tensor.
  Access operator()(const std::vector<IndexVar>& indices);

  /// Create an index expression that accesses (reads) this (scalar) tensor.
  Access operator()();

  /// Create an index expression that accesses (reads or writes) this (scalar) tensor.
  const Access operator()() const;

  /// Create an index expression that accesses (reads) this tensor.
  template <typename IndexVar,
            typename std::enable_if<!std::is_integral<IndexVar>::value,
                                    IndexVar>::type* = nullptr>
  const Access operator()(const IndexVar& index) const;

  /// Create an index expression that accesses (reads) this tensor.
  template <typename IndexVar,
            typename... IndexVars,
            typename std::enable_if<!std::is_integral<IndexVar>::value,
                                    IndexVar>::type* = nullptr>
  const Access operator()(const IndexVar index, const IndexVars&... indices) const;

  /// Create an index expression that accesses (reads) this tensor.
  template <typename IndexVar,
            typename std::enable_if<!std::is_integral<IndexVar>::value,
                                    IndexVar>::type* = nullptr>
  Access operator()(const IndexVar& index);

  /// Create an index expression that accesses (reads) this tensor.
  template <typename IndexVar,
            typename... IndexVars,
            typename std::enable_if<!std::is_integral<IndexVar>::value,
                                    IndexVar>::type* = nullptr>
  Access operator()(const IndexVar index, const IndexVars&... indices);

  /// Assign an expression to a scalar tensor.
  void operator=(const IndexExpr&);

  /* --- Compiler Methods    --- */

  /// Pack tensor into the given format
  void pack();

  /// Compile the tensor expression.
  void compile();

  /// Assemble the tensor storage, including index and value arrays.
  void assemble();

  /// Compute the given expression and put the values in the tensor storage.
  void compute();

  /// Compile, assemble and compute as needed.
  void evaluate();

  /// True if the Tensor needs to be packed.
  bool needsPack();

  /// True if the Tensor needs to be compiled.
  bool needsCompile();

  /// True if the Tensor needs to be assembled.
  bool needsAssemble();

  /// True if the Tensor needs to be computed.
  bool needsCompute();

  /// Set to true to perform the assemble and compute stages simultaneously.
  void setAssembleWhileCompute(bool assembleWhileCompute);

  /// Get the source code of the kernel functions.
  std::string getSource() const;

  /// Compile the source code of the kernel functions. This function is optional
  /// and mainly intended for experimentation. If the source code is not set
  /// then it will will be created it from the given expression.
  void compileSource(std::string source);

  /// Print the IR loops that compute the tensor's expression.
  void printComputeIR(std::ostream& stream, bool color=false,
                      bool simplify=false) const;

  /// Print the IR loops that assemble the tensor's expression.
  void printAssembleIR(std::ostream& stream, bool color=false,
                       bool simplify=false) const;

  /// Set the size of the initial index allocations.  The default size is 1MB.
  void setAllocSize(size_t allocSize);

  /// Get the size of the initial index allocations.
  size_t getAllocSize() const;

  /// Get the taco_tensor_t representation of this tensor.
  taco_tensor_t* getTacoTensorT();

  /* --- Friend Functions    --- */

  /// True iff two tensors have the same type and the same values.
  friend bool equals(const TensorBase&, const TensorBase&);

  /// True iff two TensorBase objects refer to the same tensor (TensorBase
  /// and Tensor objects are references to tensors).
  friend bool operator==(const TensorBase& a, const TensorBase& b);
  friend bool operator!=(const TensorBase& a, const TensorBase& b);

  /// True iff the address of the tensor referenced by a is smaller than the
  /// address of b.  This is arbitrary and non-deterministic, but necessary for
  /// tensor to be placed in maps.
  friend bool operator<(const TensorBase& a, const TensorBase& b);
  friend bool operator>(const TensorBase& a, const TensorBase& b);
  friend bool operator<=(const TensorBase& a, const TensorBase& b);
  friend bool operator>=(const TensorBase& a, const TensorBase& b);

  /// Print a tensor to a stream.
  friend std::ostream& operator<<(std::ostream&, const TensorBase&);
  friend std::ostream& operator<<(std::ostream&, TensorBase&);

  friend struct AccessTensorNode;

  std::vector<TensorBase> getDependentTensors();

protected:
  static std::shared_ptr<ir::Module> getHelperFunctions(
      const Format& format, Datatype ctype, const std::vector<int>& dimensions);

private:
  /* --- Compiler Methods    --- */
  void setNeedsPack(bool needsPack);
  void setNeedsCompile(bool needsCompile);
  void setNeedsAssemble(bool needsAssemble);
  void setNeedsCompute(bool needsCompute);

  void addDependentTensor(TensorBase tensor);
  void removeDependentTensor(TensorBase& tensor);
  void notifyDependentTensors();

  void syncValues();

  struct Content;
  std::shared_ptr<Content> content;

  std::shared_ptr<std::vector<char>> coordinateBuffer;
  size_t getCoordinateBufferUsed() const;
  size_t getCoordinateSize() const;
  void setCoordinateBufferUsed(size_t val);

  static std::vector<std::tuple<Format,
                                Datatype,
                                std::vector<int>,
                                std::shared_ptr<ir::Module>>> helperFunctions;
};

/// A reference to a tensor. Tensor object copies copies the reference, and
/// subsequent method calls affect both tensor references. To deeply copy a
/// tensor (for instance to change the format) compute a copy index expression
/// e.g. `A(i,j) = B(i,j).
template <typename CType>
class Tensor : public TensorBase {
public:
  /// Create a scalar
  Tensor() : TensorBase() {}

  /// Create a scalar with the given name
  explicit Tensor(std::string name) : TensorBase(name, type<CType>()) {}

  /// Create a scalar
  explicit Tensor(CType value) : TensorBase(value) {}

  /// Create a tensor with the given dimensions. The format defaults to sparse 
  /// in every mode.
  Tensor(std::vector<int> dimensions, ModeFormat modeType = ModeFormat::compressed) 
      : TensorBase(type<CType>(), dimensions) {}

  /// Create a tensor with the given dimensions and format
  Tensor(std::vector<int> dimensions, Format format)
      : TensorBase(type<CType>(), dimensions, format) {}

  /// Create a tensor with the given name, dimensions and format. The format 
  /// defaults to sparse in every mode.
  Tensor(std::string name, std::vector<int> dimensions, 
         ModeFormat modeType = ModeFormat::compressed)
      : TensorBase(name, type<CType>(), dimensions, modeType) {}

  /// Create a tensor with the given name, dimensions and format
  Tensor(std::string name, std::vector<int> dimensions, Format format)
      : TensorBase(name, type<CType>(), dimensions, format) {}

  /// Create a tensor from a TensorBase instance. The Tensor and TensorBase
  /// objects will reference the same underlying tensor so it is a shallow copy.
  Tensor(const TensorBase& tensor) : TensorBase(tensor) {
    taco_uassert(tensor.getComponentType() == type<CType>()) <<
        "Assigning TensorBase with " << tensor.getComponentType() <<
        " components to a Tensor<" << type<CType>() << ">";
  }

  CType at(const std::vector<int>& coordinate) {
    return TensorBase::at<CType>(coordinate);
  }

  /// Create an index expression that accesses (reads) this tensor.
  template <typename IndexVar,
            typename std::enable_if<!std::is_integral<IndexVar>::value,
                                    IndexVar>::type* = nullptr>
  const Access operator()(const IndexVar& index) const {
    return TensorBase::operator()({index});
  }

  /// Create an index expression that accesses (reads) this (scalar) tensor.
  Access operator()() {
    return TensorBase::operator()();
  };

  /// Create an index expression that accesses (reads or writes) this (scalar) tensor.
  const Access operator()() const {
    return TensorBase::operator()();
  };

  /// Create an index expression that accesses (reads) this tensor.
  template <typename IndexVar,
            typename... IndexVars,
            typename std::enable_if<!std::is_integral<IndexVar>::value,
                                    IndexVar>::type* = nullptr>
  const Access operator()(const IndexVar index, const IndexVars&... indices) const {
    return TensorBase::operator()({index, indices...});
  }

  /// Create an index expression that accesses (reads) this tensor.
  template <typename IndexVar,
            typename std::enable_if<!std::is_integral<IndexVar>::value,
                                    IndexVar>::type* = nullptr>
  Access operator()(const IndexVar& index) {
    return TensorBase::operator()({index});
  }

  /// Create an index expression that accesses (reads) this tensor.
  template <typename IndexVar,
            typename... IndexVars,
            typename std::enable_if<!std::is_integral<IndexVar>::value,
                                    IndexVar>::type* = nullptr>
  Access operator()(const IndexVar index, const IndexVars&... indices) {
    return TensorBase::operator()({index, indices...});
  }


  /// ScalarAccess objects are defined to simplify the sintax used for inserting
  /// scalar values stored in a tensor.
  struct ScalarAccess {
    ScalarAccess(TensorBase * tensor, const std::vector<int>& indices)
        : tensor(tensor), indices(indices) {}

    void operator=(CType scalar) {
      tensor->insert<CType>(indices, scalar);
    }

    operator CType() {
      return tensor->at<CType>(indices);
    }

    TensorBase * tensor;
    const std::vector<int> indices;
  };

  ScalarAccess operator()(const std::vector<int>& indices) {
    taco_uassert(indices.size() == (size_t)getOrder())
        << "A tensor of order " << getOrder() << " must be indexed with "
        << getOrder() << " variables, but is indexed with:  "
        << util::join(indices);
    return ScalarAccess(this, indices);
  }

  /// Create an index expression that accesses (reads) this tensor.
  template <typename Int,
            typename std::enable_if<std::is_integral<Int>::value,
                                   Int>::type* = nullptr>
  ScalarAccess operator()(const Int& index) {
    return this->operator()({index});
  }

  /// Create an index expression that accesses (reads) this tensor.
  template <typename Int,
            typename... Ints,
            typename std::enable_if<std::is_integral<Int>::value,
                                    Int>::type* = nullptr>
  ScalarAccess operator()(const Int index, const Ints&... indices) {
    return this->operator()({index, indices...});
  }

  /// Simple transpose that packs a new tensor from the values in the current tensor
  Tensor<CType> transpose(std::string name, std::vector<int> newModeOrdering) const {
    return transpose(name, newModeOrdering, getFormat());
  }
  Tensor<CType> transpose(std::vector<int> newModeOrdering) const {
    return transpose(util::uniqueName('A'), newModeOrdering);
  }
  Tensor<CType> transpose(std::vector<int> newModeOrdering, Format format) const {
    return transpose(util::uniqueName('A'), newModeOrdering, format);
  }
  Tensor<CType> transpose(std::string name, std::vector<int> newModeOrdering, Format format) const {
    // Reorder dimensions to match new mode ordering
    std::vector<int> newDimensions;
    for (int mode : newModeOrdering) {
      newDimensions.push_back(getDimensions()[mode]);
    }

    Tensor<CType> newTensor(name, newDimensions, format);
    for (const auto& value : *this) {
      std::vector<int> newCoordinate;
      for (int mode : newModeOrdering) {
        newCoordinate.push_back(value.first[mode]);
      }
      newTensor.insert(newCoordinate, value.second);
    }
    newTensor.pack();
    return newTensor;
  }

  const_iterator<int, CType> begin() const {
    return TensorBase::iterator<CType>().begin();
  }

  const_iterator<int, CType> end() const {
    return TensorBase::iterator<CType>().end();
  }

  template<typename T>
  const_iterator<T, CType> beginTyped() const {
    return TensorBase::iteratorTyped<T, CType>().begin();
  }

  template<typename T>
  const_iterator<T, CType> endTyped() const {
    return TensorBase::iteratorTyped<T, CType>().end();
  }

  /// Assign an expression to a scalar tensor.
  void operator=(const IndexExpr& expr) {TensorBase::operator=(expr);}
};


/// The file formats supported by the taco file readers and writers.
enum class FileType {
  /// .tns - The frostt sparse tensor format.  It consists of zero or more
  ///        comment lines preceded by '#', followed by any number of lines with
  ///        one coordinate/value per line.  The tensor dimensions are inferred
  ///        from the largest coordinates.
  tns,

  /// .mtx - The matrix market matrix format.  It consists of a header
  ///        line preceded by '%%', zero or more comment lines preceded by '%',
  ///        a line with the number of rows, the number of columns and the
  //         number of non-zeroes. For sparse matrix and any number of lines
  ///        with one coordinate/value per line, and for dense a list of values.
  mtx,

  /// .ttx - The tensor format derived from matrix market format. It consists
  ///        with the same header file and coordinates/values list.
  ttx,

  /// .rb  - The rutherford-boeing sparse matrix format.
  rb
};

/// Read a tensor from a file. The file format is inferred from the filename
/// and the tensor is returned packed by default.
TensorBase read(std::string filename, ModeFormat modeType, bool pack = true);

/// Read a tensor from a file. The file format is inferred from the filename
/// and the tensor is returned packed by default.
TensorBase read(std::string filename, Format format, bool pack = true);

/// Read a tensor from a file of the given file format and the tensor is
/// returned packed by default.
TensorBase read(std::string filename, FileType filetype, ModeFormat modetype,
                bool pack = true);

/// Read a tensor from a file of the given file format and the tensor is
/// returned packed by default.
TensorBase read(std::string filename, FileType filetype, Format format,
                bool pack = true);

/// Read a tensor from a stream of the given file format. The tensor is returned
/// packed by default.
TensorBase read(std::istream& stream, FileType filetype, ModeFormat modetype,
                bool pack = true);

/// Read a tensor from a stream of the given file format. The tensor is returned
/// packed by default.
TensorBase read(std::istream& stream, FileType filetype, Format format,
                bool pack = true);

/// Write a tensor to a file. The file format is inferred from the filename.
void write(std::string filename, const TensorBase& tensor);

/// Write a tensor to a file in the given file format.
void write(std::string filename, FileType filetype, const TensorBase& tensor);

/// Write a tensor to a stream in the given file format.
void write(std::ofstream& file, FileType filetype, const TensorBase& tensor);


/// Factory function to construct a compressed sparse row (CSR) matrix. The
/// arrays remain owned by the user and will not be freed by taco.

template<typename CType>
TensorBase makeCSR(const std::string& name, const std::vector<int>& dimensions,
                   int* rowptr, int* colidx, CType* vals) {
  taco_uassert(dimensions.size() == 2) << error::requires_matrix;
  Tensor<CType> tensor(name, dimensions, CSR);
  auto storage = tensor.getStorage();
  auto index = makeCSRIndex(dimensions[0], rowptr, colidx);
  storage.setIndex(index);
  storage.setValues(makeArray(vals, index.getSize(), Array::UserOwns));
  return tensor;
}

/// Factory function to construct a compressed sparse row (CSR) matrix.
template<typename T>
TensorBase makeCSR(const std::string& name, const std::vector<int>& dimensions,
                   const std::vector<int>& rowptr,
                   const std::vector<int>& colidx,
                   const std::vector<T>& vals) {
  taco_uassert(dimensions.size() == 2) << error::requires_matrix;
  Tensor<T> tensor(name, dimensions, CSR);
  auto storage = tensor.getStorage();
  storage.setIndex(makeCSRIndex(rowptr, colidx));
  storage.setValues(makeArray(vals));
  return std::move(tensor);
}

/// Get the arrays that makes up a compressed sparse row (CSR) tensor. This
/// function does not change the ownership of the arrays.
template<typename T>
void getCSRArrays(const TensorBase& tensor,
                  int** rowptr, int** colidx, T** vals) {
  taco_uassert(tensor.getFormat() == CSR) <<
  "The tensor " << tensor.getName() << " is not defined in the CSR format";
  auto storage = tensor.getStorage();
  auto index = storage.getIndex();
  
  auto rowptrArr = index.getModeIndex(1).getIndexArray(0);
  auto colidxArr = index.getModeIndex(1).getIndexArray(1);
  taco_uassert(rowptrArr.getType() == type<int>()) << error::type_mismatch;
  taco_uassert(colidxArr.getType() == type<int>()) << error::type_mismatch;
  *rowptr = static_cast<int*>(rowptrArr.getData());
  *colidx = static_cast<int*>(colidxArr.getData());
  *vals   = static_cast<T*>(storage.getValues().getData());
}

/// Factory function to construct a compressed sparse columns (CSC) matrix. The
/// arrays remain owned by the user and will not be freed by taco.
template<typename T>
TensorBase makeCSC(const std::string& name, const std::vector<int>& dimensions,
                   int* colptr, int* rowidx, T* vals) {
  taco_uassert(dimensions.size() == 2) << error::requires_matrix;
  Tensor<T> tensor(name, dimensions, CSC);
  auto storage = tensor.getStorage();
  auto index = makeCSCIndex(dimensions[1], colptr, rowidx);
  storage.setIndex(index);
  storage.setValues(makeArray(vals, index.getSize(), Array::UserOwns));
  return tensor;
}

/// Factory function to construct a compressed sparse columns (CSC) matrix.
template<typename T>
TensorBase makeCSC(const std::string& name, const std::vector<int>& dimensions,
                   const std::vector<int>& colptr,
                   const std::vector<int>& rowidx,
                   const std::vector<T>& vals) {
  taco_uassert(dimensions.size() == 2) << error::requires_matrix;
  Tensor<T> tensor(name, dimensions, CSC);
  auto storage = tensor.getStorage();
  storage.setIndex(makeCSCIndex(colptr, rowidx));
  storage.setValues(makeArray(vals));
  return std::move(tensor);
}

/// Get the arrays that makes up a compressed sparse columns (CSC) tensor. This
/// function does not change the ownership of the arrays.
template<typename T>
void getCSCArrays(const TensorBase& tensor,
                  int** colptr, int** rowidx, T** vals) {
  taco_uassert(tensor.getFormat() == CSC) <<
  "The tensor " << tensor.getName() << " is not defined in the CSC format";
  auto storage = tensor.getStorage();
  auto index = storage.getIndex();
  
  auto colptrArr = index.getModeIndex(1).getIndexArray(0);
  auto rowidxArr = index.getModeIndex(1).getIndexArray(1);
  taco_uassert(colptrArr.getType() == type<int>()) << error::type_mismatch;
  taco_uassert(rowidxArr.getType() == type<int>()) << error::type_mismatch;
  *colptr = static_cast<int*>(colptrArr.getData());
  *rowidx = static_cast<int*>(rowidxArr.getData());
  *vals   = static_cast<T*>(storage.getValues().getData());
}


/// Pack the operands in the given expression.
void packOperands(const TensorBase& tensor);

/// Iterate over the typed values of a TensorBase.
template <typename CType>
Tensor<CType> iterate(const TensorBase& tensor) {
  return Tensor<CType>(tensor);
}

/// Gets Taco's global number of threads to use for parallelism
/// This will be replaced by a scheduling language in the future
int get_taco_num_threads();

/// Sets Taco's global number of threads to use for parallelism
/// This will be replaced by a scheduling language in the future
/// Returns true if successful (ie num_threads > 0)
bool set_taco_num_threads(int num_threads);

// TensorBase template method implementations

template <typename CType>
TensorBase::TensorBase(CType val) : TensorBase(type<CType>()) {
  this->insert({}, val);
  pack();
}

template <typename CType>
void TensorBase::insert(const std::initializer_list<int>& coordinate, CType value) {
  taco_uassert(coordinate.size() == (size_t)getOrder()) <<
  "Wrong number of indices";
  taco_uassert(getComponentType() == type<CType>()) <<
  "Cannot insert a value of type '" << type<CType>() << "' " <<
  "into a tensor with component type " << getComponentType();
  notifyDependentTensors();
  if ((coordinateBuffer->size() - getCoordinateBufferUsed()) < getCoordinateSize()) {
    coordinateBuffer->resize(coordinateBuffer->size() + getCoordinateSize());
  }
  int* coordLoc = (int*)&coordinateBuffer->data()[getCoordinateBufferUsed()];
  for (int idx : coordinate) {
    *coordLoc = idx;
    coordLoc++;
  }
  TypedComponentPtr valLoc(getComponentType(), coordLoc);
  *valLoc = TypedComponentVal(getComponentType(), &value);
  setCoordinateBufferUsed(getCoordinateBufferUsed() + getCoordinateSize());
  setNeedsPack(true);
}

template <typename CType>
void TensorBase::insert(const std::vector<int>& coordinate, CType value) {
  taco_uassert(coordinate.size() == (size_t)getOrder()) <<
  "Wrong number of indices";
  taco_uassert(getComponentType() == type<CType>()) <<
    "Cannot insert a value of type '" << type<CType>() << "' " <<
    "into a tensor with component type " << getComponentType();
  notifyDependentTensors();
  if ((coordinateBuffer->size() - getCoordinateBufferUsed()) < getCoordinateSize()) {
    coordinateBuffer->resize(coordinateBuffer->size() + getCoordinateSize());
  }
  int* coordLoc = (int*)&coordinateBuffer->data()[getCoordinateBufferUsed()];
  for (int idx : coordinate) {
    *coordLoc = idx;
    coordLoc++;
  }
  TypedComponentPtr valLoc(getComponentType(), coordLoc);
  *valLoc = TypedComponentVal(getComponentType(), &value);
  setCoordinateBufferUsed(getCoordinateBufferUsed() + getCoordinateSize());
  setNeedsPack(true);
}

template <typename IndexVar,
          typename std::enable_if<!std::is_integral<IndexVar>::value,
                                  IndexVar>::type*>
const Access TensorBase::operator()(const IndexVar& index) const {
  return static_cast<const TensorBase*>(this)->operator()({index});
}

template <typename IndexVar,
          typename... IndexVars,
          typename std::enable_if<!std::is_integral<IndexVar>::value,
                                  IndexVar>::type*>
const Access TensorBase::operator()(const IndexVar index, const IndexVars&... indices) const {
  return static_cast<const TensorBase*>(this)->operator()({index, indices...});
}

template <typename IndexVar,
          typename std::enable_if<!std::is_integral<IndexVar>::value,
                                  IndexVar>::type*>
Access TensorBase::operator()(const IndexVar& index) {
  return this->operator()({index});
}

template <typename IndexVar,
          typename... IndexVars,
          typename std::enable_if<!std::is_integral<IndexVar>::value,
                                  IndexVar>::type*>
Access TensorBase::operator()(const IndexVar index, const IndexVars&... indices) {
  return this->operator()({index, indices...});
}

template <typename InputIterators>
void TensorBase::setFromComponents(const InputIterators& begin, const InputIterators& end) {
  for (InputIterators it(begin); it != end; ++it) {
    insert(it->coordinate(), it->value());
  }
}

template <typename InputIterators, typename DupFunctor>
void TensorBase::setFromComponents(const InputIterators& begin, const InputIterators& end, DupFunctor dup_func) {
  taco_not_supported_yet;
}

template <typename CType>
CType TensorBase::at(const std::vector<int>& coordinate) {
  taco_uassert(coordinate.size() == (size_t)getOrder()) <<
    "Wrong number of indices";
  taco_uassert(getComponentType() == type<CType>()) <<
    "Cannot get a value of type '" << type<CType>() << "' " <<
    "from a tensor with component type " << getComponentType();
  syncValues();

  for (auto& value : iterate<CType>(*this)) {
    if (value.first.toVector() == coordinate) {
      return value.second;
    }
  }
  return 0;
}

}
#endif
