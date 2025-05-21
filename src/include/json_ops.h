#pragma once

#ifndef JSON_OPS_H
#define JSON_OPS_H

/*!
\file
JSON handling functions
*/

/*!
\brief Extract object from JSON object
\param object   JSON object containing another object
\param name     Name of object to extract
\return Poco pointer to object
*/
inline Poco::JSON::Object::Ptr operator|(const Poco::JSON::Object::Ptr& object, const std::string& name)
{
    Poco::JSON::Object::Ptr objPtr = object->getObject(name);
    if (objPtr.isNull())
        throw Poco::RuntimeException(std::string("JSON Object contains no object ") + name, __LINE__);
    return objPtr;
}

/*!
\brief Extract list from JSON object
\param object   JSON object containing a list
\param name     Name of list to extract
\return Poco pointer to list
*/
inline Poco::JSON::Array::Ptr operator/(const Poco::JSON::Object::Ptr& object, const std::string& name)
{
    Poco::JSON::Array::Ptr arrayPtr = object->getArray(name);
    if (arrayPtr.isNull())
        throw Poco::RuntimeException(std::string("JSON Object contains no array ") + name, __LINE__);
    return arrayPtr;
}

/*!
\brief Extract object from JSON list
\param array    JSON list containing an object
\param index    List index of object to extract
\return Poco pointer to object
*/
inline Poco::JSON::Object::Ptr operator|(const Poco::JSON::Array::Ptr& array, unsigned index)
{
    Poco::JSON::Object::Ptr objPtr = array->getObject(index);
    if (objPtr.isNull())
        throw Poco::RuntimeException(std::string("JSON Array contains no index ") + std::to_string(index), __LINE__);
    return objPtr;
}

/*!
\brief Extract list from JSON list
\param array    JSON list containing a list
\param index    List index of list to extract
\return Poco pointer to list
*/
inline Poco::JSON::Array::Ptr operator/(const Poco::JSON::Array::Ptr& array, unsigned index)
{
    Poco::JSON::Array::Ptr arrayPtr = array->getArray(index);
    if (arrayPtr.isNull())
        throw Poco::RuntimeException(std::string("JSON Array contains no index ") + std::to_string(index), __LINE__);
    return arrayPtr;
}

#endif // ifndef JSON_OPS_H