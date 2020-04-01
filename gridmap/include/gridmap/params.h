#ifndef GRIDMAP_PARAMS_H
#define GRIDMAP_PARAMS_H

#include <ros/ros.h>

#include <string>

namespace gridmap
{

template <typename T>
T get_config_with_default_warn(XmlRpc::XmlRpcValue parameters, const std::string& param_name, const T& default_val,
                               const XmlRpc::XmlRpcValue::Type& xml_type)
{
    if (parameters.hasMember(param_name))
    {
        XmlRpc::XmlRpcValue& value = parameters[param_name];

        if (value.getType() != xml_type)
        {
            throw std::runtime_error(param_name + " has incorrect type");
        }

        return T(value);
    }
    else
    {
        ROS_WARN_STREAM("Using default value for " << param_name << ": " << default_val);
        return default_val;
    }
}

template <typename T, size_t size>
std::array<T, size> get_config_list_with_default(XmlRpc::XmlRpcValue parameters, const std::string& param_name,
                                                 const std::array<T, size>& default_val,
                                                 const XmlRpc::XmlRpcValue::Type& xml_type)
{
    if (parameters.hasMember(param_name))
    {
        XmlRpc::XmlRpcValue& value = parameters[param_name];
        if (value.getType() != XmlRpc::XmlRpcValue::TypeArray)
        {
            throw std::runtime_error(param_name + " has incorrect type, expects a TypeArray");
        }
        if (value.size() != size)
        {
            throw std::runtime_error(param_name + " has incorrect size");
        }
        std::array<T, size> params_array;
        for (int32_t i = 0; i < value.size(); ++i)
        {
            if (value[i].getType() != xml_type)
            {
                throw std::runtime_error(param_name + " element has incorrect type");
            }
            else
            {
                params_array[i] = static_cast<T>(value[i]);
            }
        }
        return params_array;
    }
    else
    {
        return default_val;
    }
}
}  // namespace gridmap

#endif