/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2017, James R. Barlow (https://github.com/jbarlow83/)
 */

#include <sstream>
#include <iostream>
#include <iomanip>
#include <cctype>

#include <qpdf/Constants.h>
#include <qpdf/Types.h>
#include <qpdf/DLL.h>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFXRefEntry.hh>
#include <qpdf/PointerHolder.hh>
#include <qpdf/Buffer.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "pikepdf.h"

/*
New type table

Encode Python type to C++ type
Decode C++ type to Python

Definite native:
Null - None
Boolean - bool
Integer - int
Real - Decimal

Uncertain:
String - str or bytes ?

Convertible:
Name - Name('/thing')
Operator - Operator('Do')
Array - list / iterable
Dictionary - dict
Stream - Stream()

Object API

qpdf.Object.Typename()  <-- class-ish name, static method in reality
tries to coerce input to Pdf object of typename, or fails

qpdf.Object.new() <--- tries to create a Pdf object from its input with when
possible without ambiguity

Boolean <- bool
Integer <- int
Real <- decimal.Decimal, float
String <- str, bytes
    this will need help from Pdf doc encoding

Array <- list, tuple
Dictionary <- dict, Mapping

Stream <- present as qpdf.Object.Stream({dictionary}, stream=<...>)

when does Dictionary.__setitem__ coerce its value to a Pdf object? on input
or serialization
    probably on input, fail first

that means __setitem__ needs to recursively coerce

should be able to assign python objects and have them mapped to appropriate
objects - or


// qpdf.Object.Boolean(True) <-- class-ish name, static method in reality
// instead of
// qpdf.Object.new(True)  <--- when possible without ambiguity
// strings:
// qpdf.Object.Name("")
// qpdf.Object.new("/Name"?)

// Then repr becomes...
// or should each object be type-decorated?
qpdf.Object.Dictionary({
    "/Type": "/Page",
    "/MediaBox": [],
    "/Contents": <qpdf.Object.Stream>,
})


*/


class PyParserCallbacks : public QPDFObjectHandle::ParserCallbacks {
public:
    using QPDFObjectHandle::ParserCallbacks::ParserCallbacks;

    void handleObject(QPDFObjectHandle h) override {
        PYBIND11_OVERLOAD_PURE_NAME(
            void,
            QPDFObjectHandle::ParserCallbacks,
            "handle_object", /* Python name */
            handleObject, /* C++ name */
            h
        );
    }

    void handleEOF() override {
        PYBIND11_OVERLOAD_PURE_NAME(
            void,
            QPDFObjectHandle::ParserCallbacks,
            "handle_eof", /* Python name */
            handleEOF, /* C++ name; trailing comma needed for macro */
        );
    }
};


class OperandGrouper : public QPDFObjectHandle::ParserCallbacks {
public:
    OperandGrouper(const std::string& operators)
        : parsing_inline_image(false), count(0)
    {
        std::istringstream f(operators);
        std::string s;
        while (std::getline(f, s, ' ')) {
            this->whitelist.insert(s);
        }
    }
    virtual ~OperandGrouper() {}

    void handleObject(QPDFObjectHandle obj) override
    {
        this->count++;
        if (obj.getTypeCode() == QPDFObject::object_type_e::ot_operator) {
            std::string op = obj.getOperatorValue();

            // If we have a whitelist and this operator is not on the whitelist,
            // discard it and all the tokens we collected
            if (!this->whitelist.empty()) {
                if (op[0] == 'q' || op[0] == 'Q') {
                    // We have token with multiple stack push/pops
                    if (this->whitelist.count("q") == 0 && this->whitelist.count("Q") == 0) {
                        this->tokens.clear();
                        return;
                    }
                } else if (this->whitelist.count(op) == 0) {
                    this->tokens.clear();
                    return;
                }
            }
            if (op == "BI") {
                this->parsing_inline_image = true;
            } else if (this->parsing_inline_image) {
                if (op == "ID") {
                    this->inline_metadata = this->tokens;
                } else if (op == "EI") {
                    auto PdfInlineImage = py::module::import("pikepdf").attr("PdfInlineImage");
                    auto kwargs = py::dict();
                    kwargs["image_data"] = this->tokens.at(0);
                    kwargs["image_object"] = this->inline_metadata;
                    auto iimage = PdfInlineImage(**kwargs);

                    // Package as list with single element for consistency
                    auto iimage_list = py::list();
                    iimage_list.append(iimage);

                    auto instruction = py::make_tuple(
                        iimage_list,
                        QPDFObjectHandle::newOperator("INLINE IMAGE")
                    );
                    this->instructions.append(instruction);

                    this->parsing_inline_image = false;
                    this->inline_metadata.clear();
                }
            } else {
                py::list operand_list = py::cast(this->tokens);
                auto instruction = py::make_tuple(operand_list, obj);
                this->instructions.append(instruction);
            }
            this->tokens.clear();
        } else {
            this->tokens.push_back(obj);
        }
    }

    void handleEOF() override
    {
        if (!this->tokens.empty())
            this->warning = "Unexpected end of stream";
    }

    py::list getInstructions()
    {
        return this->instructions;
    }

    std::string getWarning()
    {
        return this->warning;
    }

private:
    std::set<std::string> whitelist;
    std::vector<QPDFObjectHandle> tokens;
    bool parsing_inline_image;
    std::vector<QPDFObjectHandle> inline_metadata;
    py::list instructions;
    uint count;
    std::string warning;
};


size_t list_range_check(QPDFObjectHandle& h, int index)
{
    if (!h.isArray())
        throw py::value_error("object is not an array");
    if (index < 0)
        index += h.getArrayNItems(); // Support negative indexing
    if (!(0 <= index && index < h.getArrayNItems()))
        throw py::index_error("index out of range");
    return (size_t)index;
}


bool objecthandle_equal(QPDFObjectHandle& self, QPDFObjectHandle& other)
{
    StackGuard sg(" objecthandle_equal");

    // Uninitialized objects are never equal
    if (!self.isInitialized() || !other.isInitialized())
        return false;

    // Indirect objects (objid != 0) with the same obj-gen are equal and same owner
    // are equal (in fact, they are identical; they reference the same underlying
    // QPDFObject, even if the handles are different).
    // This lets us compare deeply nested and cyclic structures without recursing
    // into them.
    if (self.getObjectID() != 0
        && other.getObjectID() != 0
        && self.getOwningQPDF() == other.getOwningQPDF()) {
        return self.getObjGen() == other.getObjGen();
    }

    // If 'self' is a numeric type, convert both to Decimal objects
    // and compare them as such.
    if (self.getTypeCode() == QPDFObject::object_type_e::ot_integer ||
        self.getTypeCode() == QPDFObject::object_type_e::ot_real ||
        self.getTypeCode() == QPDFObject::object_type_e::ot_boolean) {
        try {
            auto a = decimal_from_pdfobject(self);
            auto b = decimal_from_pdfobject(other);
            py::object pyresult = a.attr("__eq__")(b);
            bool result = pyresult.cast<bool>();
            return result;
        } catch (py::type_error) {
            return false;
        }
    }

    // Apart from numeric types, disimilar types are never equal
    if (self.getTypeCode() != other.getTypeCode())
        return false;

    switch (self.getTypeCode()) {
        case QPDFObject::object_type_e::ot_null:
            return true; // Both must be null
        case QPDFObject::object_type_e::ot_boolean:
            return self.getBoolValue() == other.getBoolValue();
        case QPDFObject::object_type_e::ot_name:
            return self.getName() == other.getName();
        case QPDFObject::object_type_e::ot_operator:
            return self.getOperatorValue() == other.getOperatorValue();
        case QPDFObject::object_type_e::ot_string:
        {
            // We don't know what encoding the string is in
            // This ensures UTF-16 coded ASCII strings will compare equal to
            // UTF-8/ASCII coded.
            return self.getStringValue() == other.getStringValue() ||
                self.getUTF8Value() == other.getUTF8Value();
        }
        case QPDFObject::object_type_e::ot_array:
        {
            // Call operator==() on each element of the arrays, meaning this
            // recurses into this function
            return (self.getArrayAsVector() == other.getArrayAsVector());
        }
        case QPDFObject::object_type_e::ot_dictionary:
        {
            // Call operator==() on each element of the arrays, meaning this
            // recurses into this function
            return (self.getDictAsMap() == other.getDictAsMap());
        }
        default:
            break;
    }
    return false;
}


bool operator==(const QPDFObjectHandle& self, const QPDFObjectHandle& other)
{
    // A lot of functions in QPDFObjectHandle are not tagged const where they
    // should be, but are const-safe
    return objecthandle_equal(
        const_cast<QPDFObjectHandle &>(self),
        const_cast<QPDFObjectHandle &>(other));
}


QPDFObjectHandle object_get_key(QPDFObjectHandle& h, std::string const& key)
{
    if (!h.isDictionary() && !h.isStream())
        throw py::value_error("object is not a dictionary or a stream");
    QPDFObjectHandle dict = h.isStream() ? h.getDict() : h;
    if (!dict.hasKey(key))
        throw py::key_error(key);
    return dict.getKey(key);
}


void init_object(py::module& m)
{
    py::enum_<QPDFObject::object_type_e>(m, "ObjectType")
        .value("uninitialized", QPDFObject::object_type_e::ot_uninitialized)
        .value("reserved", QPDFObject::object_type_e::ot_reserved)
        .value("null", QPDFObject::object_type_e::ot_null)
        .value("boolean", QPDFObject::object_type_e::ot_boolean)
        .value("integer", QPDFObject::object_type_e::ot_integer)
        .value("real", QPDFObject::object_type_e::ot_real)
        .value("string", QPDFObject::object_type_e::ot_string)
        .value("name", QPDFObject::object_type_e::ot_name)
        .value("array", QPDFObject::object_type_e::ot_array)
        .value("dictionary", QPDFObject::object_type_e::ot_dictionary)
        .value("stream", QPDFObject::object_type_e::ot_stream)
        .value("operator", QPDFObject::object_type_e::ot_operator)
        .value("inlineimage", QPDFObject::object_type_e::ot_inlineimage);


    py::class_<Buffer, PointerHolder<Buffer>>(m, "Buffer", py::buffer_protocol())
        .def_buffer([](Buffer &b) -> py::buffer_info {
            return py::buffer_info(
                b.getBuffer(),
                sizeof(unsigned char),
                py::format_descriptor<unsigned char>::format(),
                1,
                { b.getSize() },
                { sizeof(unsigned char) }
            );
        });

    static QPDFObjectHandle static_handle;
    py::class_<QPDFObjectHandle>(m, "Object")
        .def_static("new",
            [](bool b) {
                return QPDFObjectHandle::newBool(b);
            }
        )
        .def_static("new",
            [](int n) {
                return QPDFObjectHandle::newInteger(n);
            }
        )
        .def_static("new",
            [](double f) {
                return QPDFObjectHandle::newReal(f, 0); // default to six decimals
            }
        )
        .def_static("new",
            [](std::string s) {
                return QPDFObjectHandle::newString(s); // TO DO: warn about /Name
            }
        )
        .def_static("new",
            [](py::none none) {
                return QPDFObjectHandle::newNull();
            }
        )
        .def_property_readonly("type_code", &QPDFObjectHandle::getTypeCode)
        .def_property_readonly("type_name", &QPDFObjectHandle::getTypeName)
        // .def_property_readonly("owner", &QPDFObjectHandle::getOwningQPDF,
        //     "Return the QPDF object that owns an indirect object.  Returns None for a direct object."
        // )
        .def("check_owner",
            [](QPDFObjectHandle &h, std::shared_ptr<QPDF> possible_owner) {
                return (h.getOwningQPDF() == possible_owner.get());
            },
            "Test if this object is owned by the indicated *possible_owner*."
        )
        .def_property_readonly("is_indirect", &QPDFObjectHandle::isIndirect)
        .def("__repr__", &objecthandle_repr)
        .def("__hash__",
            [](QPDFObjectHandle &self) -> py::int_ {
                py::object hash = py::module::import("builtins").attr("hash");

                //Objects which compare equal must have the same hash value
                switch (self.getTypeCode()) {
                    case QPDFObject::object_type_e::ot_null:
                        return py::int_(0);
                    case QPDFObject::object_type_e::ot_boolean:
                        return py::int_(self.getBoolValue());
                    case QPDFObject::object_type_e::ot_integer:
                        return py::int_(self.getIntValue());
                    case QPDFObject::object_type_e::ot_real:
                        return hash(decimal_from_pdfobject(self));
                    case QPDFObject::object_type_e::ot_string:
                    {
                        return hash(py::bytes(self.getUTF8Value()));
                    }
                    case QPDFObject::object_type_e::ot_name:
                        return hash(py::bytes(self.getName()));
                    case QPDFObject::object_type_e::ot_operator:
                        return hash(py::bytes(self.getOperatorValue()));
                    case QPDFObject::object_type_e::ot_array:
                    case QPDFObject::object_type_e::ot_dictionary:
                    case QPDFObject::object_type_e::ot_stream:
                    case QPDFObject::object_type_e::ot_inlineimage:
                        throw py::value_error("Can't hash mutable object");
                    default:
                        break;
                }
                throw std::logic_error("don't know how to hash this");
            }
        )
        .def("__eq__",
            [](QPDFObjectHandle &self, QPDFObjectHandle &other) {
                return (self == other); // overloaded
            }
        )
        .def("__eq__",
            [](QPDFObjectHandle &self, long long other) {
                /* Objects of different numeric types are expected to compare equal */
                if (!self.isInitialized())
                    return false;
                if (self.getTypeCode() == QPDFObject::object_type_e::ot_integer)
                    return self.getIntValue() == other;
                return false;
            }
        )
        .def("__eq__",
            [](QPDFObjectHandle &self, py::str other) {
                std::string utf8_other = other.cast<std::string>();
                switch (self.getTypeCode()) {
                    case QPDFObject::object_type_e::ot_string:
                        return self.getUTF8Value() == utf8_other;
                    case QPDFObject::object_type_e::ot_name:
                        return self.getName() == utf8_other;
                    default:
                        return false;
                }
            }
        )
        .def("__eq__",
            [](QPDFObjectHandle &self, py::bytes other) {
                std::string bytes_other = other.cast<std::string>();
                switch (self.getTypeCode()) {
                    case QPDFObject::object_type_e::ot_string:
                        return self.getStringValue() == bytes_other;
                    case QPDFObject::object_type_e::ot_name:
                        return self.getName() == bytes_other;
                    default:
                        return false;
                }
            }
        )
        .def("__lt__",
            [](QPDFObjectHandle &self, QPDFObjectHandle &other) {
                if (!self.isInitialized() || !other.isInitialized())
                    throw py::type_error("comparison involving an uninitialized object");
                if (self.getTypeCode() == QPDFObject::object_type_e::ot_integer ||
                    self.getTypeCode() == QPDFObject::object_type_e::ot_real) {
                    try {
                        auto a = decimal_from_pdfobject(self);
                        auto b = decimal_from_pdfobject(other);
                        py::object pyresult = a.attr("__lt__")(b);
                        bool result = pyresult.cast<bool>();
                        return result;
                    } catch (py::type_error) {
                        throw py::type_error("comparison undefined");
                    }
                }
                throw py::type_error("comparison undefined");
            }
        )
        .def("__lt__",
            [](QPDFObjectHandle &self, long long other) {
                if (!self.isInitialized())
                    throw py::type_error("comparison involving an uninitialized object");
                if (self.getTypeCode() == QPDFObject::object_type_e::ot_integer ||
                    self.getTypeCode() == QPDFObject::object_type_e::ot_real) {
                    try {
                        auto a = decimal_from_pdfobject(self);
                        auto b = py::int_(other);
                        py::object pyresult = a.attr("__lt__")(b);
                        bool result = pyresult.cast<bool>();
                        return result;
                    } catch (py::type_error) {
                        throw py::type_error("comparison undefined");
                    }
                }
                throw py::type_error("comparison undefined");
            }
        )
        .def("__len__",
            [](QPDFObjectHandle &h) {
                if (h.isDictionary())
                    return (Py_ssize_t)h.getDictAsMap().size(); // getKeys constructs a new object, so this is better
                else if (h.isArray())
                    return (Py_ssize_t)h.getArrayNItems();
                throw py::value_error("length not defined for object");
            }
        )
        .def("__getitem__",
            [](QPDFObjectHandle &h, std::string const& key) {
                return object_get_key(h, key);
            }
        )
        .def("__setitem__",
            [](QPDFObjectHandle &h, std::string const& key, QPDFObjectHandle &value) {
                if (!h.isDictionary() && !h.isStream())
                    throw py::value_error("object is not a dictionary or a stream");

                // For streams, the actual dictionary is attached to stream object
                QPDFObjectHandle dict = h.isStream() ? h.getDict() : h;

                // if (value.isScalar() || value.isStream()) {
                //     dict.replaceKey(key, value);
                //     return;
                // }

                // try {
                //     auto copy = value.shallowCopy();
                //     copy.makeDirect();
                // } catch (std::exception &e) {
                //     throw py::value_error(e.what());
                // }
                dict.replaceKey(key, value);
            },
            "assign dictionary key to new object",
            py::keep_alive<1, 3>()
        )
        .def("__setitem__",
            [](QPDFObjectHandle &h, std::string const& key, py::object &pyvalue) {
                if (!h.isDictionary() && !h.isStream())
                    throw py::value_error("object is not a dictionary or a stream");

                // For streams, the actual dictionary is attached to stream object
                QPDFObjectHandle dict = h.isStream() ? h.getDict() : h;

                auto value = objecthandle_encode(pyvalue);
                // A stream dictionary has no owner, so use the stream object in this comparison
                dict.replaceKey(key, value);
            }
        )
        .def("__delitem__",
            [](QPDFObjectHandle &h, std::string const& key) {
                if (!h.isDictionary())
                    throw py::value_error("object is not a dictionary");

                if (!h.hasKey(key))
                    throw py::key_error(key);

                h.removeKey(key);
            },
            "delete a dictionary key"
        )
        .def("__getattr__",
            [](QPDFObjectHandle &h, std::string const& name) {
                QPDFObjectHandle value;
                std::string key = "/" + name;
                try {
                    value = object_get_key(h, key);
                } catch (py::key_error &e) {
                    if (std::isupper(name[0]))
                        throw py::attr_error(e.what());
                    else
                        throw py::attr_error(name);
                }
                return value;
            },
            "attribute lookup name"
        )
        .def("__setattr__",
            [](QPDFObjectHandle &h, std::string const& name, py::object &pyvalue) {
                if (!h.isDictionary() && !h.isStream())
                    throw py::attr_error("object is not a dictionary or a stream");
                QPDFObjectHandle dict = h.isStream() ? h.getDict() : h;
                std::string key = "/" + name;
                auto value = objecthandle_encode(pyvalue);
                dict.replaceKey(key, value);
            },
            "attribute access"
        )
        .def("__dir__",
            [](QPDFObjectHandle &h) {
                py::list result;
                py::object obj = py::cast(h);
                py::object class_keys = obj.attr("__class__").attr("__dict__").attr("keys")();
                for (auto attr: class_keys) {
                    result.append(attr);
                }
                if (h.isDictionary() || h.isStream()) {
                    for (auto key_attr: h.getKeys()) {
                        std::string s = key_attr.substr(1);
                        result.append(py::str(s));
                    }
                }
                return result;
            }
        )
        .def("get",
            [](QPDFObjectHandle &h, std::string const& key, py::object default_) {
                QPDFObjectHandle value;
                try {
                    value = object_get_key(h, key);
                } catch (py::key_error &e) {
                    return default_;
                }
                return py::cast(value);
            },
            "for dictionary objects, behave as dict.get(key, default=None)",
            py::arg("key"),
            py::arg("default_") = py::none()
        )
        .def("keys", &QPDFObjectHandle::getKeys)
        .def("__contains__",
            [](QPDFObjectHandle &h, std::string const& key) {
                if (h.isDictionary()) {
                    return h.hasKey(key);
                }
                if (h.isStream()) {
                    return h.getDict().hasKey(key);
                }
                throw py::value_error("__contains__ not defined for object type");
            }
        )
        .def("as_list", &QPDFObjectHandle::getArrayAsVector)
        .def("as_dict", &QPDFObjectHandle::getDictAsMap)
        .def("as_int", &QPDFObjectHandle::getIntValue)
        .def("__int__", &QPDFObjectHandle::getIntValue)
        .def("__float__",
            [](QPDFObjectHandle &h) {
                if (h.isNumber())
                    return h.getNumericValue();
                throw py::type_error("Not a numeric object");
            }
        )
        .def("as_bool", &QPDFObjectHandle::getBoolValue)
        .def("decode", objecthandle_decode, "convert to nearest Python object")
        .def("__str__",
            [](QPDFObjectHandle &h) -> py::str {
                if (h.isName())
                    return h.getName();
                else if (h.isOperator())
                    return h.getOperatorValue();
                else if (h.isReal())
                    return h.getRealValue();
                else if (h.isInteger())
                    return py::str(py::int_(h.getIntValue()));
                return h.getUTF8Value();
            }
        )
        .def("__bytes__",
            [](QPDFObjectHandle &h) {
                if (h.isName())
                    return py::bytes(h.getName());
                if (h.isStream()) {
                    PointerHolder<Buffer> buf = h.getStreamData();
                    // py::bytes will make a copy of the buffer, so releasing is fine
                    return py::bytes((const char*)buf->getBuffer(), buf->getSize());
                }
                return py::bytes(h.getStringValue());
            }
        )
        .def("__getitem__",
            [](QPDFObjectHandle &h, int index) {
                size_t u_index = list_range_check(h, index);
                return h.getArrayItem(u_index);
            }
        )
        .def("__setitem__",
            [](QPDFObjectHandle &h, int index, QPDFObjectHandle &value) {
                size_t u_index = list_range_check(h, index);
                h.setArrayItem(u_index, value);
            }
        )
        .def("__setitem__",
            [](QPDFObjectHandle &h, int index, py::object &pyvalue) {
                size_t u_index = list_range_check(h, index);
                auto value = objecthandle_encode(pyvalue);
                h.setArrayItem(u_index, value);
            }
        )
        .def("__delitem__",
            [](QPDFObjectHandle &h, int index) {
                size_t u_index = list_range_check(h, index);
                h.eraseItem(u_index);
            }
        )
        .def_property("stream_dict",
            &QPDFObjectHandle::getDict, &QPDFObjectHandle::replaceDict,
            py::return_value_policy::reference_internal
        )
        .def("get_stream_buffer",
            [](QPDFObjectHandle &h) {
                PointerHolder<Buffer> phbuf = h.getStreamData();
                return phbuf;
            },
            "Return a buffer protocol buffer describing the decoded stream"
        )
        .def("get_raw_stream_buffer",
            [](QPDFObjectHandle &h) {
                PointerHolder<Buffer> phbuf = h.getRawStreamData();
                return phbuf;
            },
            "Return a buffer protocol buffer describing the raw, encoded stream"
        )
        .def("read_bytes",
            [](QPDFObjectHandle &h) {
                PointerHolder<Buffer> buf = h.getStreamData();
                // py::bytes will make a copy of the buffer, so releasing is fine
                return py::bytes((const char*)buf->getBuffer(), buf->getSize());
            },
            py::return_value_policy::take_ownership,
            "Decode and read the content stream associated with this object"
        )
        .def("read_raw_bytes",
            [](QPDFObjectHandle &h) {
                PointerHolder<Buffer> buf = h.getRawStreamData();
                // py::bytes will make a copy of the buffer, so releasing is fine
                return py::bytes((const char*)buf->getBuffer(), buf->getSize());
            },
            py::return_value_policy::take_ownership,
            "Read the content stream associated with this object without decoding"
        )
        .def("write",
            [](QPDFObjectHandle &h, py::bytes data, QPDFObjectHandle &filter, QPDFObjectHandle &decode_parms) {
                std::string sdata = data;
                h.replaceStreamData(sdata, filter, decode_parms);
            },
            py::keep_alive<1, 3>(),
            py::keep_alive<1, 4>(),
            "Replace the content stream with data, which is compressed according to filter and decode params"
        )
        .def_property_readonly("images",
            [](QPDFObjectHandle &h) {
                if (!h.isPageObject())
                    throw py::type_error("Not a Page");
                //Maybe?
                //h.getOwningQPDF().pushInheritedAttributesToPage();
                return h.getPageImages();
            }
        )
        .def("_inline_image_raw_bytes",
            [](QPDFObjectHandle &h) {
                return py::bytes(h.getInlineImageValue());
            }
        )
        .def("page_contents_add",
            [](QPDFObjectHandle &h, QPDFObjectHandle &contents, bool prepend) {
                if (!h.isPageObject())
                    throw py::type_error("Not a Page");
                h.addPageContents(contents, prepend);
            },
            "Append or prepend to an existing page's content stream.",
            py::arg("contents"),
            py::arg("prepend") = false,
            py::keep_alive<1, 2>()
        )
        .def("page_contents_coalesce", &QPDFObjectHandle::coalesceContentStreams)
        .def_property_readonly("_objgen",
            [](QPDFObjectHandle &h) {
                auto objgen = h.getObjGen();
                return std::pair<int, int>(objgen.getObj(), objgen.getGen());
            }
        )
        .def_static("parse",
            [](std::string const& stream, std::string const& description) {
                return QPDFObjectHandle::parse(stream, description);
            },
            "Parse text PostScript into PDF objects.",
            py::arg("stream"),
            py::arg("description") = ""
        )
        .def_static("parse",
            [](py::bytes &stream, std::string const& description) {
                std::string s = stream;
                return QPDFObjectHandle::parse(stream, description);
            },
            "Parse binary PostScript into PDF objects.",
            py::arg("stream"),
            py::arg("description") = ""
        )
        .def("_parse_page_contents",
            &QPDFObjectHandle::parsePageContents,
            "Helper for parsing page contents; use ``pikepdf.parse_content_stream``."
        )
        .def("_parse_page_contents_grouped",
            [](QPDFObjectHandle &h, std::string const& whitelist) {
                OperandGrouper og(whitelist);
                h.parsePageContents(&og);
                return og.getInstructions();
            }
        )
        .def_static("_parse_stream",
            &QPDFObjectHandle::parseContentStream,
            "Helper for parsing PDF content stream; use ``pikepdf.parse_content_stream``."
        )
        .def_static("_parse_stream_grouped",
            [](QPDFObjectHandle &h, std::string const& whitelist) {
                OperandGrouper og(whitelist);
                QPDFObjectHandle::parseContentStream(h, &og);
                if (!og.getWarning().empty()) {
                    auto warn = py::module::import("warnings").attr("warn");
                    warn(og.getWarning());
                }
                return og.getInstructions();
            }
        )
        .def("unparse",
            [](QPDFObjectHandle &h) -> py::bytes {
                return h.unparse();
            },
            "Convert PDF objects into PostScript, without resolving indirect objects."
        )
        .def("unparse_resolved",
            [](QPDFObjectHandle &h) -> py::bytes {
                return h.unparseResolved();
            },
            "Convert PDF objects into PostScript, and resolve referenced objects when possible."
        )
        .def("_repr_pdf_singlepage",
            [](QPDFObjectHandle &page) -> py::object {
                if (!page.isPageObject())
                    return py::none();
                QPDF q;
                q.emptyPDF();
                q.setSuppressWarnings(true);

                QPDFObjectHandle page_copy = q.copyForeignObject(page);
                q.addPage(page_copy, true);

                QPDFWriter w(q);
                w.setOutputMemory();
                w.write();
                std::unique_ptr<Buffer> output_buffer(w.getBuffer());
                auto output = py::bytes(
                    (const char*)output_buffer->getBuffer(),
                    output_buffer->getSize());
                return output;
            },
            "Render as PDF - for Jupyter/IPython"
        )
        .def("_repr_svg_",
            [](QPDFObjectHandle &page) -> py::object {
                if (!page.isPageObject())
                    return py::none();
                auto page_to_svg = py::module::import("pikepdf._pdfimage").attr("page_to_svg");
                return page_to_svg(page);
            }
        )
        ; // end of QPDFObjectHandle bindings

    m.def("_new_boolean", &QPDFObjectHandle::newBool, "Construct a PDF Boolean object");
    m.def("_new_integer", &QPDFObjectHandle::newInteger, "Construct a PDF Integer object");
    m.def("_new_real",
        [](const std::string& value) {
            return QPDFObjectHandle::newReal(value);
        },
        "Construct a PDF Real value, that is, a decimal number"
    );
    m.def("_new_real",
        [](double value, uint places) {
            return QPDFObjectHandle::newReal(value, places);
        },
        "Construct PDF real",
        py::arg("value"),
        py::arg("places") = 0
    );
    m.def("_new_name",
        [](const std::string& s) {
            if (s.at(0) != '/')
                throw py::value_error("Name objects must begin with '/'");
            if (s.length() < 2)
                throw py::value_error("Name must be at least one character long");
            return QPDFObjectHandle::newName(s);
        },
        "Create a Name from a string. Must begin with '/'. All other characters except null are valid."
    );
    m.def("_new_string",
        [](const std::string& s) {
            return QPDFObjectHandle::newString(s);
        },
        "Construct a PDF String object."
    );
    m.def("_new_array",
        [](py::iterable iterable) {
            return QPDFObjectHandle::newArray(array_builder(iterable));
        },
        "Construct a PDF Array object from an iterable of PDF objects or types that can be coerced to PDF objects."
    );
    m.def("_new_dictionary",
        [](py::dict dict) {
            return QPDFObjectHandle::newDictionary(dict_builder(dict));
        },
        "Construct a PDF Dictionary from a mapping of PDF objects or Python types that can be coerced to PDF objects."
    );
    m.def("Stream",
        [](std::shared_ptr<QPDF> owner, py::bytes data) {
            std::string s = data;
            return QPDFObjectHandle::newStream(owner.get(), data); // This makes a copy of the data
        },
        "Construct a PDF Stream object from binary data",
        py::keep_alive<0, 1>() // returned object references the owner
    );
    m.def("Stream",
        [](std::shared_ptr<QPDF> owner, py::iterable content_stream) {
            std::stringstream data;

            for (auto handle_command : content_stream) {
                py::tuple command = py::reinterpret_borrow<py::tuple>(handle_command);

                if (command.size() != 2)
                    throw py::value_error("Each item in stream data must be a tuple(operands, operator)");

                py::object operands = command[0];
                py::object operator_ = command[1];
                for (auto operand : operands) {
                    QPDFObjectHandle h = objecthandle_encode(operand);
                    data << h.unparse();
                    data << " ";
                }
                data << objecthandle_encode(operator_).unparse();
                data << "\n";
            }
            return QPDFObjectHandle::newStream(owner.get(), data.str());
        },
        "Construct a PDF Stream object from a list of operand-operator tuples [((operands,), operator)]",
        py::keep_alive<0, 1>() // returned object references the owner
    );
    m.def("Operator",
        [](const std::string& op) {
            return QPDFObjectHandle::newOperator(op);
        },
        "Construct a PDF Operator object for use in content streams"
    );
    m.def("Null", &QPDFObjectHandle::newNull,
        "Construct a PDF Null object"
    );

    py::class_<QPDFObjectHandle::ParserCallbacks, PyParserCallbacks> parsercallbacks(m, "StreamParser");
    parsercallbacks
        .def(py::init<>())
        .def("handle_object", &QPDFObjectHandle::ParserCallbacks::handleObject)
        .def("handle_eof", &QPDFObjectHandle::ParserCallbacks::handleEOF);

    m.def("_encode",
        [](py::none none) {
            // Without this shim, no template is selected for _encode(None)
            // causing it to return a type error
            return QPDFObjectHandle::newNull();
        }
    );
    m.def("_encode",
        [](py::handle handle) {
            return objecthandle_encode(handle);
        }
    );
    m.def("_decode", &objecthandle_decode);

} // init_object
