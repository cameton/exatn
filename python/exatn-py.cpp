#include "DriverClient.hpp"
#include "exatn.hpp"

#include "pybind11/detail/common.h"
#include <pybind11/complex.h>
#include <pybind11/eigen.h>
#include <pybind11/functional.h>
#include <pybind11/iostream.h>
#include <pybind11/numpy.h>
#include <pybind11/operators.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

#include "pybind11/pybind11.h"
#include "talshxx.hpp"
#include "tensor_method.hpp"

namespace py = pybind11;
using namespace exatn;
using namespace exatn::numerics;
using namespace pybind11::literals;

/**
 * This module provides the necessary bindings for the ExaTN classes and
 * functionality to be used within python. The various classes are bound with
 * pybind11 and, when this is included in CMake compilation, can be used to
 * write python scripts which leverage the ExaTN functionality.
 */

/**
  Trampoline class for abstract virtual functions in TensorOperation
*/

class PyTensorOperation : public exatn::numerics::TensorOperation {
public:
  /* Inherit the constructors */
  using TensorOperation::TensorOperation;
  void printIt() { PYBIND11_OVERLOAD(void, TensorOperation, printIt, ); }
  bool isSet() { PYBIND11_OVERLOAD_PURE(bool, TensorOperation, isSet, ); }
};

using TensorFunctor = talsh::TensorFunctor<Identifiable>;

template <typename NumericType>
class NumpyTensorFunctorCppWrapper : public TensorFunctor {
protected:
  std::function<void(py::array_t<NumericType> &buffer)> _functor;
  py::array initialData;
  bool initialDataProvided = false;

public:
  NumpyTensorFunctorCppWrapper(
      std::function<void(py::array_t<NumericType> &buffer)> functor)
      : _functor(functor) {}
  NumpyTensorFunctorCppWrapper(py::array &buffer)
      : initialData(buffer), initialDataProvided(true) {}
  const std::string name() const override {
    return "numpy_tensor_functor_cpp_wrapper";
  }
  const std::string description() const override { return ""; }
  virtual void pack(BytePacket &packet) override {}
  virtual void unpack(BytePacket &packet) override {}

  int apply(talsh::Tensor &local_tensor) override {
    py::gil_scoped_release release;
    unsigned int nd = local_tensor.getRank();
    std::vector<int> dims_vec(nd);
    auto dims = local_tensor.getDimExtents(nd);
    for (int i = 0; i < nd; i++) {
      dims_vec[i] = dims[i];
    }

    NumericType *elements;
    local_tensor.getDataAccessHost(&elements);

    if (initialDataProvided) {
      // If initial data is provided as a numpy array,
      // then I want to flatten it, and set it on the elements data
      std::vector<std::size_t> flattened{local_tensor.getVolume()};
      assert(local_tensor.getVolume() == initialData.size());
      initialData.resize(flattened);
      auto constelements =
          reinterpret_cast<const NumericType *>(initialData.data());
      for (int i = 0; i < local_tensor.getVolume(); i++) {
        elements[i] = constelements[i]; //.cast<double>();
      }
    } else {
      auto cap = py::capsule(
          elements, [](void *v) { /* deleter, I do not own this... */ });
      py::gil_scoped_acquire acquire;
      auto arr = py::array_t<NumericType>(dims_vec, elements, cap);
      _functor(arr);
      py::gil_scoped_release r;
    }
    return 0;
  }
};

template <typename T> struct TypeToTensorElementType;
template <> struct TypeToTensorElementType<float> {
  static TensorElementType type;
  // = TensorElementType::COMPLEX32;
};
template <> struct TypeToTensorElementType<double> {
  static TensorElementType type;
};
template <> struct TypeToTensorElementType<std::complex<double>> {
  static TensorElementType type;
};
TensorElementType TypeToTensorElementType<float>::type =
    TensorElementType::REAL32;
TensorElementType TypeToTensorElementType<double>::type =
    TensorElementType::REAL64;
TensorElementType TypeToTensorElementType<std::complex<double>>::type =
    TensorElementType::COMPLEX64;

template <typename NumericType>
bool createTensorWithData(exatn::NumServer &n, const std::string name,
                          py::array_t<NumericType> &data) {
  auto shape = data.shape();
  std::vector<std::size_t> dims(data.ndim());
  for (int i = 0; i < data.ndim(); i++) {
    dims[i] = shape[i];
  }

  auto tensor_el_type = TypeToTensorElementType<NumericType>::type;
  auto created =
      n.createTensor(name, tensor_el_type, exatn::numerics::TensorShape(dims));
  assert(created);
  auto functor =
      std::make_shared<NumpyTensorFunctorCppWrapper<NumericType>>(data);
  return n.transformTensor(name, functor);
}

template <typename NumericType>
bool generalTransformWithData(
    exatn::NumServer &n, const std::string &name,
    std::function<void(py::array_t<NumericType> &buffer)> f) {
  auto functor = std::make_shared<NumpyTensorFunctorCppWrapper<NumericType>>(f);
  return n.transformTensor(name, functor);
}

PYBIND11_MODULE(_pyexatn, m) {
  m.doc() = "Python bindings for ExaTN.";

  py::class_<BytePacket>(m, "BytePacket", "")
      .def_readwrite("base_addr", &BytePacket::base_addr, "")
      .def_readwrite("size_bytes", &BytePacket::size_bytes, "");

  py::class_<exatn::rpc::DriverClient,
             std::shared_ptr<exatn::rpc::DriverClient>>(m, "DriverClient", "")
      .def("interpretTAProL", &exatn::rpc::DriverClient::interpretTAProL, "")
      .def("registerTensorMethod",
           &exatn::rpc::DriverClient::registerTensorMethod, "")
      .def("getResults", &exatn::rpc::DriverClient::getResults, "")
      .def("shutdown", &exatn::rpc::DriverClient::shutdown, "");

  /**
      Numerics namespace python class bindings
   */

  py::class_<exatn::numerics::TensorOperation,
             std::shared_ptr<exatn::numerics::TensorOperation>,
             PyTensorOperation>(m, "TensorOperation", "")
      .def("printIt", &exatn::numerics::TensorOperation::printIt, "")
      .def("isSet", &exatn::numerics::TensorOperation::isSet, "")
      .def("getNumOperands", &exatn::numerics::TensorOperation::getNumOperands,
           "")
      .def("getNumOperandsSet",
           &exatn::numerics::TensorOperation::getNumOperandsSet, "")
      .def("getTensorOperandHash",
           &exatn::numerics::TensorOperation::getTensorOperandHash, "")
      .def("getTensorOperand",
           &exatn::numerics::TensorOperation::getTensorOperand, "")
      .def("setTensorOperand",
           &exatn::numerics::TensorOperation::setTensorOperand, "")
      .def("getNumScalars", &exatn::numerics::TensorOperation::getNumScalars,
           "")
      .def("getNumScalarsSet",
           &exatn::numerics::TensorOperation::getNumScalarsSet, "")
      .def("getScalar", &exatn::numerics::TensorOperation::getScalar, "")
      .def("setScalar", &exatn::numerics::TensorOperation::setScalar, "")
      .def("getIndexPattern",
           &exatn::numerics::TensorOperation::getIndexPattern, "")
      .def("setIndexPattern",
           &exatn::numerics::TensorOperation::setIndexPattern, "");

  py::class_<exatn::numerics::TensorOpAdd,
             std::shared_ptr<exatn::numerics::TensorOpAdd>,
             exatn::numerics::TensorOperation>(m, "TensorOpAdd", "")
      .def(py::init<>())
      .def("isSet", &exatn::numerics::TensorOpAdd::isSet, "")
      .def("createNew", &exatn::numerics::TensorOpAdd::createNew, "");

  py::class_<exatn::numerics::TensorOpContract,
             std::shared_ptr<exatn::numerics::TensorOpContract>,
             exatn::numerics::TensorOperation>(m, "TensorOpContract", "")
      .def(py::init<>())
      .def("isSet", &exatn::numerics::TensorOpContract::isSet, "")
      .def("createNew", &exatn::numerics::TensorOpContract::createNew, "");

  py::class_<exatn::numerics::TensorOpCreate,
             std::shared_ptr<exatn::numerics::TensorOpCreate>,
             exatn::numerics::TensorOperation>(m, "TensorOpCreate", "")
      .def(py::init<>())
      .def("isSet", &exatn::numerics::TensorOpCreate::isSet, "")
      .def("createNew", &exatn::numerics::TensorOpCreate::createNew, "");

  py::class_<exatn::numerics::TensorOpDestroy,
             std::shared_ptr<exatn::numerics::TensorOpDestroy>,
             exatn::numerics::TensorOperation>(m, "TensorOpDestroy", "")
      .def(py::init<>())
      .def("isSet", &exatn::numerics::TensorOpDestroy::isSet, "")
      .def("createNew", &exatn::numerics::TensorOpDestroy::createNew, "");

  py::class_<exatn::numerics::TensorOpFactory>(m, "TensorOpFactory", "")
      .def("registerTensorOp",
           &exatn::numerics::TensorOpFactory::registerTensorOp, "")
      .def("get", &exatn::numerics::TensorOpFactory::get,
           py::return_value_policy::reference, "")
      .def("createTensorOpShared",
           &exatn::numerics::TensorOpFactory::createTensorOpShared, "");

  py::class_<exatn::numerics::TensorOpTransform,
             std::shared_ptr<exatn::numerics::TensorOpTransform>,
             exatn::numerics::TensorOperation>(m, "TensorOpTransform", "")
      .def("isSet", &exatn::numerics::TensorOpTransform::isSet, "")
      .def("createNew", &exatn::numerics::TensorOpTransform::createNew, "");

  py::class_<exatn::numerics::TensorNetwork,
             std::shared_ptr<exatn::numerics::TensorNetwork>>(
      m, "TensorNetwork", "")
      .def(py::init<>())
      .def(py::init<const std::string>())
      .def(py::init<const std::string, std::shared_ptr<exatn::numerics::Tensor>,
                    const std::vector<exatn::numerics::TensorLeg>>())
      .def(py::init<const std::string, const std::string,
                    const std::map<std::string,
                                   std::shared_ptr<exatn::numerics::Tensor>>>())
      .def("printIt", &exatn::numerics::TensorNetwork::printIt, "")
      .def("getName", &exatn::numerics::TensorNetwork::getName, "")
      .def("isEmpty", &exatn::numerics::TensorNetwork::isEmpty, "")
      .def("isExplicit", &exatn::numerics::TensorNetwork::isExplicit, "")
      .def("finalize", &exatn::numerics::TensorNetwork::finalize,
           "check_validity"_a = false, "")
      .def("isFinalized", &exatn::numerics::TensorNetwork::isFinalized, "")
      .def("getNumTensors", &exatn::numerics::TensorNetwork::getNumTensors, "")
      .def("getName", &exatn::numerics::TensorNetwork::getName, "")
      .def("getTensor", &exatn::numerics::TensorNetwork::getTensor, "")
      .def("appendTensor",
           (bool (exatn::numerics::TensorNetwork::*)(
               unsigned int, std::shared_ptr<exatn::numerics::Tensor>,
               const std::vector<exatn::numerics::TensorLeg> &)) &
               exatn::numerics::TensorNetwork::appendTensor,
           "")
      .def("appendTensor",
           (bool (exatn::numerics::TensorNetwork::*)(
               unsigned int, std::shared_ptr<exatn::numerics::Tensor>,
               const std::vector<std::pair<unsigned int, unsigned int>> &,
               const std::vector<exatn::LegDirection> &)) &
               exatn::numerics::TensorNetwork::appendTensor,
           "")
      .def("reoderOutputModes",
           &exatn::numerics::TensorNetwork::reoderOutputModes, "")
      .def("deleteTensor", &exatn::numerics::TensorNetwork::deleteTensor, "")
      .def("mergeTensors", &exatn::numerics::TensorNetwork::mergeTensors, "");

  py::class_<exatn::numerics::VectorSpace>(m, "VectorSpace", "")
      .def(py::init<DimExtent>())
      .def(py::init<DimExtent, const std::string>())
      .def(py::init<DimExtent, const std::string,
                    const std::vector<SymmetryRange>>())
      .def("getDimension", &exatn::numerics::VectorSpace::getDimension, "")
      .def("printIt", &exatn::numerics::VectorSpace::printIt, "")
      .def("getName", &exatn::numerics::VectorSpace::getName, "")
      .def("getSymmetrySubranges",
           &exatn::numerics::VectorSpace::getSymmetrySubranges, "")
      .def("registerSymmetrySubrange",
           &exatn::numerics::VectorSpace::registerSymmetrySubrange, "")
      .def("getRegisteredId", &exatn::numerics::VectorSpace::getRegisteredId,
           "");

  py::class_<exatn::numerics::Subspace>(m, "Subspace", "")
      .def(py::init<exatn::numerics::VectorSpace *, DimOffset, DimOffset>())
      .def(py::init<exatn::numerics::VectorSpace *,
                    std::pair<DimOffset, DimOffset>>())
      .def(py::init<exatn::numerics::VectorSpace *, DimOffset, DimOffset,
                    const std::string &>())
      .def(py::init<exatn::numerics::VectorSpace *,
                    std::pair<DimOffset, DimOffset>, const std::string &>())
      .def("getDimension", &exatn::numerics::Subspace::getDimension, "")
      .def("printIt", &exatn::numerics::Subspace::printIt, "")
      .def("getLowerBound", &exatn::numerics::Subspace::getLowerBound, "")
      .def("getUpperBound", &exatn::numerics::Subspace::getUpperBound, "")
      .def("getBounds", &exatn::numerics::Subspace::getBounds, "")
      .def("getName", &exatn::numerics::Subspace::getName, "")
      .def("getVectorSpace", &exatn::numerics::Subspace::getVectorSpace, "")
      .def("getRegisteredId", &exatn::numerics::Subspace::getRegisteredId, "");

  py::class_<exatn::numerics::Tensor, std::shared_ptr<exatn::numerics::Tensor>>(
      m, "Tensor", "")
      .def(py::init<std::string>())
      .def(py::init<std::string, exatn::numerics::TensorShape,
                    exatn::numerics::TensorSignature>())
      .def(py::init<std::string, exatn::numerics::TensorShape>())
      .def(py::init<std::string, exatn::numerics::Tensor,
                    exatn::numerics::Tensor,
                    std::vector<exatn::numerics::TensorLeg>>())
      // templated constructor requires integral type for TensorShape -
      // need a definition for each templated constructor and (common) integral
      // type
      .def(py::init<std::string, std::initializer_list<int>,
                    std::initializer_list<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::initializer_list<short>,
                    std::initializer_list<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::initializer_list<long>,
                    std::initializer_list<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::initializer_list<unsigned int>,
                    std::initializer_list<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::initializer_list<unsigned short>,
                    std::initializer_list<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::initializer_list<unsigned long>,
                    std::initializer_list<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::vector<char>,
                    std::vector<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::vector<short>,
                    std::vector<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::vector<long>,
                    std::vector<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::vector<unsigned int>,
                    std::vector<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::vector<unsigned short>,
                    std::vector<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::vector<unsigned long>,
                    std::vector<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<std::string, std::vector<int>>())
      .def(py::init<std::string, std::vector<short>>())
      .def(py::init<std::string, std::vector<long>>())
      .def(py::init<std::string, std::vector<unsigned int>>())
      .def(py::init<std::string, std::vector<unsigned short>>())
      .def(py::init<std::string, std::vector<unsigned long>>())
      .def(py::init<std::string, std::initializer_list<int>>())
      .def(py::init<std::string, std::initializer_list<short>>())
      .def(py::init<std::string, std::initializer_list<long>>())
      .def(py::init<std::string, std::initializer_list<unsigned int>>())
      .def(py::init<std::string, std::initializer_list<unsigned short>>())
      .def(py::init<std::string, std::initializer_list<unsigned long>>())
      .def("printIt", &exatn::numerics::Tensor::printIt, "")
      .def("getName", &exatn::numerics::Tensor::getName, "")
      .def("getRank", &exatn::numerics::Tensor::getRank, "")
      .def("getShape", &exatn::numerics::Tensor::getShape, "")
      .def("getSignature", &exatn::numerics::Tensor::getSignature, "")
      .def("getDimExtent", &exatn::numerics::Tensor::getDimExtent, "")
      .def("getDimExtents", &exatn::numerics::Tensor::getDimExtents, "")
      .def("getDimSpaceId", &exatn::numerics::Tensor::getDimSpaceId, "")
      .def("getDimSubspaceId", &exatn::numerics::Tensor::getDimSubspaceId, "")
      .def("getDimSpaceAttr", &exatn::numerics::Tensor::getDimSpaceAttr, "")
      .def("deleteDimension", &exatn::numerics::Tensor::deleteDimension, "")
      .def("appendDimension",
           (void (exatn::numerics::Tensor::*)(std::pair<SpaceId, SubspaceId>,
                                              DimExtent)) &
               exatn::numerics::Tensor::appendDimension,
           "")
      .def("appendDimension",
           (void (exatn::numerics::Tensor::*)(DimExtent)) &
               exatn::numerics::Tensor::appendDimension,
           "")
      .def("getTensorHash", &exatn::numerics::Tensor::getTensorHash, "");

  py::enum_<exatn::LegDirection>(m, "LegDirection")
      .value("UNDIRECT", exatn::LegDirection::UNDIRECT)
      .value("INWARD", exatn::LegDirection::INWARD)
      .value("OUTWARD", exatn::LegDirection::OUTWARD)
      .export_values();

  py::enum_<exatn::TensorOpCode>(m, "TensorOpCode")
      .value("NOOP", exatn::TensorOpCode::NOOP)
      .value("CREATE", exatn::TensorOpCode::CREATE)
      .value("DESTROY", exatn::TensorOpCode::DESTROY)
      .value("TRANSFORM", exatn::TensorOpCode::TRANSFORM)
      .value("ADD", exatn::TensorOpCode::ADD)
      .value("CONTRACT", exatn::TensorOpCode::CONTRACT)
      .export_values();

  py::class_<exatn::numerics::TensorLeg,
             std::shared_ptr<exatn::numerics::TensorLeg>>(m, "TensorLeg", "")
      // Specify the default LegDirection argument for this constructor
      .def(py::init<unsigned int, unsigned int, exatn::LegDirection>(),
           "tensor_id"_a, "dimensn_id"_a,
           "direction"_a = exatn::LegDirection::UNDIRECT)
      .def(py::init<const TensorLeg>())
      .def("printIt", &exatn::numerics::TensorLeg::printIt, "")
      .def("getTensorId", &exatn::numerics::TensorLeg::getTensorId, "")
      .def("getDimensionId", &exatn::numerics::TensorLeg::getDimensionId, "")
      .def("getDirection", &exatn::numerics::TensorLeg::getDirection, "")
      .def("resetConnection", &exatn::numerics::TensorLeg::resetConnection, "")
      .def("resetTensorId", &exatn::numerics::TensorLeg::resetTensorId, "")
      .def("resetDimensionId", &exatn::numerics::TensorLeg::resetDimensionId,
           "")
      .def("resetDirection", &exatn::numerics::TensorLeg::resetDirection, "");

  py::class_<exatn::NumServer, std::shared_ptr<exatn::NumServer>>(
      m, "NumServer", "")
      .def(py::init<>())
      .def("reconfigureTensorRuntime",
           &exatn::NumServer::reconfigureTensorRuntime, "")
      .def("registerTensorMethod", &exatn::NumServer::registerTensorMethod, "")
      .def("getTensorMethod", &exatn::NumServer::getTensorMethod, "")
      .def("registerExternalData", &exatn::NumServer::registerExternalData, "")
      .def("getExternalData", &exatn::NumServer::getExternalData, "")
      .def("openScope", &exatn::NumServer::openScope, "")
      .def("closeScope", &exatn::NumServer::closeScope, "")
      .def("getVectorSpace", &exatn::NumServer::getVectorSpace, "")
      .def("destroyVectorSpace",
           (void (exatn::NumServer::*)(const std::string &)) &
               exatn::NumServer::destroyVectorSpace,
           "")
      .def("destroyVectorSpace",
           (void (exatn::NumServer::*)(SpaceId)) &
               exatn::NumServer::destroyVectorSpace,
           "")
      .def("getSubspace", &exatn::NumServer::getSubspace, "")
      .def("destroySubspace",
           (void (exatn::NumServer::*)(const std::string &)) &
               exatn::NumServer::destroySubspace,
           "")
      .def("destroySubspace",
           (void (exatn::NumServer::*)(SubspaceId)) &
               exatn::NumServer::destroySubspace,
           "")
      .def("submit",
           (void (exatn::NumServer::*)(
               std::shared_ptr<exatn::numerics::TensorOperation>)) &
               exatn::NumServer::submit,
           "")
      .def("submit",
           (void (exatn::NumServer::*)(exatn::numerics::TensorNetwork &)) &
               exatn::NumServer::submit,
           "")
      .def("submit",
           (void (exatn::NumServer::*)(
               std::shared_ptr<exatn::numerics::TensorNetwork>)) &
               exatn::NumServer::submit,
           "")
      .def("sync",
           (bool (exatn::NumServer::*)(const exatn::numerics::Tensor &, bool)) &
               exatn::NumServer::sync,
           "")
      .def("sync",
           (bool (exatn::NumServer::*)(exatn::numerics::TensorOperation &,
                                       bool)) &
               exatn::NumServer::sync,
           "")
      .def(
          "sync",
          (bool (exatn::NumServer::*)(exatn::numerics::TensorNetwork &, bool)) &
              exatn::NumServer::sync,
          "")
      .def("sync",
           (bool (exatn::NumServer::*)(const std::string &, bool)) &
               exatn::NumServer::sync,
           "")
      .def("getTensorRef", &exatn::NumServer::getTensorRef, "")
      .def(
          "createTensor",
          [](exatn::NumServer &n, const std::string name,
             std::vector<std::size_t> dims) {
            bool created = false;
            created = n.createTensor(name, TensorElementType::REAL64,
                                     exatn::numerics::TensorShape(dims));
            assert(created);
            return;
          },
          "")
      .def(
          "createTensor",
          [](exatn::NumServer &n, const std::string name) {
            bool created = false;
            created = n.createTensor(name, TensorElementType::REAL64);
            assert(created);
            return;
          },
          "")

      .def("createTensor", &createTensorWithData<double>, "")
      .def("createTensor", &createTensorWithData<std::complex<double>>, "")
      .def("initTensor", &exatn::NumServer::initTensor<float>, "")
      .def("initTensor", &exatn::NumServer::initTensor<int>, "")
      .def("initTensor", &exatn::NumServer::initTensor<double>, "")
      .def("initTensor", &exatn::NumServer::initTensor<std::complex<double>>,
           "")
      .def("transformTensor", &exatn::NumServer::transformTensor, "")
      .def("transformTensor", &generalTransformWithData<double>, "")
      .def("transformTensor", &generalTransformWithData<std::complex<double>>,
           "")
      .def("destroyTensor", &exatn::NumServer::destroyTensor, "")
      //   .def("addTensors", &exatn::NumServer::addTensors<float>, "")
      //   .def("addTensors", &exatn::NumServer::addTensors<double>, "")
      //   .def("addTensors", &exatn::NumServer::addTensors<int>, "")
      //   .def("addTensors",
      //   &exatn::NumServer::addTensors<std::complex<double>>, "")
      //   .def("contractTensors", &exatn::NumServer::contractTensors<float>,
      //   "") .def("contractTensors",
      //   &exatn::NumServer::contractTensors<double>, "")
      //   .def("contractTensors", &exatn::NumServer::contractTensors<int>, "")
      //   .def("contractTensors",
      //   &exatn::NumServer::contractTensors<std::complex<double>>, "")
      .def("evaluateTensorNetwork", &exatn::NumServer::evaluateTensorNetwork,
           "");

  py::class_<exatn::numerics::TensorConn>(m, "TensorConn", "")
      .def(py::init<std::shared_ptr<exatn::numerics::Tensor>, unsigned int,
                    const std::vector<exatn::numerics::TensorLeg>>())
      .def("printIt", &exatn::numerics::TensorConn::printIt, "")
      .def("getNumLegs", &exatn::numerics::TensorConn::getNumLegs, "")
      .def("getTensorId", &exatn::numerics::TensorConn::getTensorId, "")
      .def("getTensor", &exatn::numerics::TensorConn::getTensor, "")
      .def("getTensorLeg", &exatn::numerics::TensorConn::getTensorLeg, "")
      .def("getTensorLegs", &exatn::numerics::TensorConn::getTensorLegs, "")
      .def("getDimExtent", &exatn::numerics::TensorConn::getDimExtent, "")
      .def("getDimSpaceAttr", &exatn::numerics::TensorConn::getDimSpaceAttr, "")
      .def("resetLeg", &exatn::numerics::TensorConn::resetLeg, "")
      .def("deleteLeg", &exatn::numerics::TensorConn::deleteLeg, "")
      .def("deleteLegs", &exatn::numerics::TensorConn::deleteLegs, "")
      .def("appendLeg",
           (void (exatn::numerics::TensorConn::*)(
               std::pair<SpaceId, SubspaceId>, DimExtent,
               exatn::numerics::TensorLeg)) &
               exatn::numerics::TensorConn::appendLeg,
           "")
      .def("appendLeg",
           (void (exatn::numerics::TensorConn::*)(DimExtent,
                                                  exatn::numerics::TensorLeg)) &
               exatn::numerics::TensorConn::appendLeg,
           "");

  py::class_<exatn::numerics::TensorShape>(m, "TensorShape", "")
      .def(py::init<>())
      .def(py::init<std::initializer_list<int>>())
      .def(py::init<std::initializer_list<short>>())
      .def(py::init<std::initializer_list<long>>())
      .def(py::init<std::initializer_list<unsigned int>>())
      .def(py::init<std::initializer_list<unsigned short>>())
      .def(py::init<std::initializer_list<unsigned long>>())
      .def(py::init<std::vector<int>>())
      .def(py::init<std::vector<short>>())
      .def(py::init<std::vector<long>>())
      .def(py::init<std::vector<unsigned int>>())
      .def(py::init<std::vector<unsigned short>>())
      .def(py::init<std::vector<unsigned long>>())
      .def("printIt", &exatn::numerics::TensorShape::printIt, "")
      .def("getRank", &exatn::numerics::TensorShape::getRank, "")
      .def("getDimExtent", &exatn::numerics::TensorShape::getDimExtent, "")
      .def("getDimExtents", &exatn::numerics::TensorShape::getDimExtents, "")
      .def("resetDimension", &exatn::numerics::TensorShape::resetDimension, "")
      .def("deleteDimension", &exatn::numerics::TensorShape::deleteDimension,
           "")
      .def("appendDimension", &exatn::numerics::TensorShape::appendDimension,
           "");

  py::class_<exatn::numerics::TensorSignature>(m, "TensorSignature", "")
      .def(py::init<>())
      .def(py::init<std::initializer_list<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<const std::vector<std::pair<SpaceId, SubspaceId>>>())
      .def(py::init<unsigned int>())
      .def("printIt", &exatn::numerics::TensorSignature::printIt, "")
      .def("getRank", &exatn::numerics::TensorSignature::getRank, "")
      .def("getDimSpaceId", &exatn::numerics::TensorSignature::getDimSpaceId,
           "")
      .def("getDimSubspaceId",
           &exatn::numerics::TensorSignature::getDimSubspaceId, "")
      .def("resetDimension", &exatn::numerics::TensorSignature::resetDimension,
           "")
      .def("deleteDimension",
           &exatn::numerics::TensorSignature::deleteDimension, "")
      .def("appendDimension",
           &exatn::numerics::TensorSignature::appendDimension, "")
      .def("getDimSpaceAttr",
           &exatn::numerics::TensorSignature::getDimSpaceAttr, "");

  py::class_<exatn::numerics::SubspaceRegEntry>(m, "SubspaceRegEntry")
      .def(py::init<std::shared_ptr<Subspace>>());

  py::class_<exatn::numerics::SubspaceRegister>(m, "SubspaceRegister", "")
      .def(py::init<>())
      .def("registerSubspace",
           &exatn::numerics::SubspaceRegister::registerSubspace, "")
      .def("getSubspace",
           (const exatn::numerics::Subspace *(
               exatn::numerics::SubspaceRegister::*)(SubspaceId) const) &
               exatn::numerics::SubspaceRegister::getSubspace,
           "")
      .def(
          "getSubspace",
          (const exatn::numerics::Subspace *(
              exatn::numerics::SubspaceRegister::*)(const std::string &)const) &
              exatn::numerics::SubspaceRegister::getSubspace,
          "");

  py::class_<exatn::numerics::SpaceRegEntry>(m, "SpaceRegEntry")
      .def(py::init<std::shared_ptr<exatn::numerics::VectorSpace>>());

  py::class_<exatn::numerics::SpaceRegister>(m, "SpaceRegister")
      .def(py::init<>())
      .def("registerSpace", &exatn::numerics::SpaceRegister::registerSpace, "")
      .def("registerSubspace",
           &exatn::numerics::SpaceRegister::registerSubspace, "")
      .def("getSubspace", &exatn::numerics::SpaceRegister::getSubspace, "")
      .def("getSpace",
           (const exatn::numerics::VectorSpace *(
               exatn::numerics::SpaceRegister::*)(SpaceId) const) &
               exatn::numerics::SpaceRegister::getSpace,
           "")
      .def("getSpace",
           (const exatn::numerics::VectorSpace *(
               exatn::numerics::SpaceRegister::*)(const std::string &)const) &
               exatn::numerics::SpaceRegister::getSpace,
           "");

  py::class_<exatn::numerics::SymmetryRange>(m, "SymmetryRange");

  py::class_<exatn::numerics::SpaceBasis>(m, "SpaceBasis")
      .def(py::init<DimExtent>())
      .def(py::init<DimExtent,
                    const std::vector<exatn::numerics::SymmetryRange>>())
      .def("printIt", &exatn::numerics::SpaceBasis::printIt, "")
      .def("getDimension", &exatn::numerics::SpaceBasis::getDimension, "")
      .def("getSymmetrySubranges",
           &exatn::numerics::SpaceBasis::getSymmetrySubranges, "")
      .def("registerSymmetrySubrange",
           &exatn::numerics::SpaceBasis::registerSymmetrySubrange, "");

  py::class_<exatn::numerics::BasisVector>(m, "BasisVector")
      .def(py::init<SubspaceId>())
      .def("printIt", &exatn::numerics::BasisVector::printIt, "");

  /**
   ExaTN module definitions
  */
  m.def("Initialize", (void (*)()) & exatn::initialize,
        "Initialize the exatn framework.");
  m.def(
      "getDriverClient",
      [](const std::string name) -> std::shared_ptr<exatn::rpc::DriverClient> {
        return exatn::getService<exatn::rpc::DriverClient>(name);
      },
      "");
  m.def("Finalize", &exatn::finalize, "Finalize the framework");

  m.def(
      "getNumServer", []() { return exatn::numericalServer; },
      py::return_value_policy::reference, "");

  m.def(
      "createVectorSpace",
      [](const std::string &space_name, DimExtent space_dim) {
        const VectorSpace *space;
        return exatn::numericalServer->createVectorSpace(space_name, space_dim,
                                                         &space);
      },
      py::return_value_policy::reference, "");

  m.def(
      "getVectorSpace",
      [](const std::string &space_name) {
        return exatn::numericalServer->getVectorSpace(space_name);
      },
      py::return_value_policy::reference, "");

  m.def(
      "createSubspace",
      [](const std::string &subspace_name, const std::string &space_name,
         std::pair<DimOffset, DimOffset> bounds) {
        const Subspace *subspace;
        return numericalServer->createSubspace(subspace_name, space_name,
                                               bounds, &subspace);
      },
      py::return_value_policy::reference, "");
  m.def(
      "getSubspace",
      [](const std::string &subspace_name) {
        return exatn::numericalServer->getSubspace(subspace_name);
      },
      py::return_value_policy::reference, "");

  // TensorNetwork *network is the network to append to, and TensorNetwork
  // append_network is the network that will be appended to *network PyBind
  // cannot bind this function simply within the TensorNetwork class due to the
  // && argument
  m.def(
      "appendTensorNetwork",
      [](TensorNetwork *network, TensorNetwork append_network,
         const std::vector<std::pair<unsigned int, unsigned int>> &pairing) {
        return network->appendTensorNetwork(std::move(append_network), pairing);
      },
      "");
}
