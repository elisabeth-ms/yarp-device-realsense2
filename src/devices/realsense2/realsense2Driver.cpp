/*
 * Copyright (C) 2006-2018 Istituto Italiano di Tecnologia (IIT)
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms of the
 * BSD-3-Clause license. See the accompanying LICENSE file for details.
 */

#include <cmath>
#include <algorithm>
#include <iomanip>
#include <cstdint>

#include <yarp/os/Value.h>

#include <librealsense2/rsutil.h>
#include "realsense2Driver.h"

using namespace yarp::dev;
using namespace yarp::sig;
using namespace yarp::os;

using namespace std;

constexpr char accuracy       [] = "accuracy";
constexpr char clipPlanes     [] = "clipPlanes";
constexpr char depthRes       [] = "depthResolution";
constexpr char rgbRes         [] = "rgbResolution";

static std::string get_device_information(const rs2::device& dev)
{

    std::stringstream ss;
    ss << "Device information: " << std::endl;

    for (int i = 0; i < static_cast<int>(RS2_CAMERA_INFO_COUNT); i++)
    {
        rs2_camera_info info_type = static_cast<rs2_camera_info>(i);
        ss << "  " << std::left << std::setw(20) << info_type << " : ";

        if (dev.supports(info_type))
            ss << dev.get_info(info_type) << std::endl;
        else
            ss << "N/A" << std::endl;
    }
    return ss.str();
}


static void print_supported_options(const rs2::sensor& sensor)
{
    // Sensors usually have several options to control their properties
    //  such as Exposure, Brightness etc.

    if (sensor.is<rs2::depth_sensor>())
        yInfo() << "Depth sensor supports the following options:\n";
    else
        yInfo() << "RGB camera supports the following options:\n";

    // The following loop shows how to iterate over all available options
    // Starting from 0 until RS2_OPTION_COUNT (exclusive)
    for (int i = 0; i < static_cast<int>(RS2_OPTION_COUNT); i++)
    {
        rs2_option option_type = static_cast<rs2_option>(i);
        //SDK enum types can be streamed to get a string that represents them

        // To control an option, use the following api:

        // First, verify that the sensor actually supports this option
        if (sensor.supports(option_type))
        {
            std::cout << "  " << option_type;
            std::cout << std::endl;

            // Get a human readable description of the option
            const char* description = sensor.get_option_description(option_type);
            std::cout << "       Description   : " << description << std::endl;

            // Get the current value of the option
            float current_value = sensor.get_option(option_type);
            std::cout << "       Current Value : " << current_value << std::endl;
        }
    }

    std::cout<<std::endl;
}

static bool setOption(rs2_option option,const rs2::sensor* sensor, float value)
{

    if (!sensor)
    {
        return false;
    }

    // First, verify that the sensor actually supports this option
    if (!sensor->supports(option))
    {
        yError() << "The option" << option << "is not supported by this sensor";
        return false;
    }

    // To set an option to a different value, we can call set_option with a new value
    try
    {
        sensor->set_option(option, value);
    }
    catch (const rs2::error& e)
    {
        // Some options can only be set while the camera is streaming,
        // and generally the hardware might fail so it is good practice to catch exceptions from set_option
        yError() << "Failed to set option " << option << ". (" << e.what() << ")";
        return false;
    }
    return true;
}

static bool getOption(rs2_option option,const rs2::sensor *sensor, float &value)
{
    if (!sensor)
    {
        return false;
    }

    // First, verify that the sensor actually supports this option
    if (!sensor->supports(option))
    {
        yError() << "The option" << option << "is not supported by this sensor";
        return false;
    }

    // To set an option to a different value, we can call set_option with a new value
    try
    {
        value = sensor->get_option(option);
    }
    catch (const rs2::error& e)
    {
        // Some options can only be set while the camera is streaming,
        // and generally the hardware might fail so it is good practice to catch exceptions from set_option
        yError() << "Failed to get option " << option << ". (" << e.what() << ")";
        return false;
    }
    return true;
}

static int pixFormatToCode(const rs2_format p)
{
    switch(p)
    {
    case (RS2_FORMAT_RGB8):
        return VOCAB_PIXEL_RGB;

    case (RS2_FORMAT_BGR8):
        return VOCAB_PIXEL_BGR;

    case (RS2_FORMAT_Z16):
        return VOCAB_PIXEL_MONO16;

    case (RS2_FORMAT_DISPARITY16):
        return VOCAB_PIXEL_MONO16;

    case (RS2_FORMAT_RGBA8):
        return VOCAB_PIXEL_RGBA;

    case (RS2_FORMAT_BGRA8):
        return VOCAB_PIXEL_BGRA;

    case (RS2_FORMAT_Y8):
        return VOCAB_PIXEL_MONO;

    case (RS2_FORMAT_Y16):
        return VOCAB_PIXEL_MONO16;;

    case (RS2_FORMAT_RAW16):
        return VOCAB_PIXEL_MONO16;

    case (RS2_FORMAT_RAW8):
        return VOCAB_PIXEL_MONO;
    default:
        return VOCAB_PIXEL_INVALID;

    }
}

static size_t bytesPerPixel(const rs2_format format)
{
    size_t bytes_per_pixel = 0;
    switch (format)
    {
    case RS2_FORMAT_RAW8:
    case RS2_FORMAT_Y8:
        bytes_per_pixel = 1;
        break;
    case RS2_FORMAT_Z16:
    case RS2_FORMAT_DISPARITY16:
    case RS2_FORMAT_Y16:
    case RS2_FORMAT_RAW16:
        bytes_per_pixel = 2;
        break;
    case RS2_FORMAT_RGB8:
    case RS2_FORMAT_BGR8:
        bytes_per_pixel = 3;
        break;
    case RS2_FORMAT_RGBA8:
    case RS2_FORMAT_BGRA8:
        bytes_per_pixel = 4;
        break;
    default:
        break;
    }
    return bytes_per_pixel;
}


realsense2Driver::realsense2Driver() : m_depth_sensor(nullptr), m_color_sensor(nullptr),
                                       m_paramParser(nullptr), m_depthRegistration(false),
                                       m_verbose(false), m_period(0)
{

    m_params_map =
    {
        {accuracy,       RGBDSensorParamParser::RGBDParam(accuracy,        1)},
        {clipPlanes,     RGBDSensorParamParser::RGBDParam(clipPlanes,      2)},
        {depthRes,       RGBDSensorParamParser::RGBDParam(depthRes,        2)},
        {rgbRes,         RGBDSensorParamParser::RGBDParam(rgbRes,          2)}

    };

    m_depthRegistration = true;

    m_paramParser = new RGBDSensorParamParser();

    // realsense SDK already provides them
    m_paramParser->depthIntrinsic.isOptional = true;
    m_paramParser->rgbIntrinsic.isOptional   = true;
    m_paramParser->isOptionalExtrinsic       = true;

    m_supportedFeatures.push_back(YARP_FEATURE_EXPOSURE);
    m_supportedFeatures.push_back(YARP_FEATURE_WHITE_BALANCE);
    m_supportedFeatures.push_back(YARP_FEATURE_GAIN);
    m_supportedFeatures.push_back(YARP_FEATURE_FRAME_RATE);
    m_supportedFeatures.push_back(YARP_FEATURE_SHARPNESS);
    m_supportedFeatures.push_back(YARP_FEATURE_HUE);
    m_supportedFeatures.push_back(YARP_FEATURE_SATURATION);
}

realsense2Driver::~realsense2Driver()
{
    if (m_paramParser)
    {
        delete m_paramParser;
        m_paramParser = nullptr;
    }

    return;
}

bool realsense2Driver::pipelineStartup()
{
    try
    {
        m_pipeline.start(m_cfg);
    }
    catch (const rs2::error& e)
    {
        yError() << "realsense2Driver: failed to start the pipeline:"<< "(" << e.what() << ")";
        return false;
    }
    return true;
}

bool realsense2Driver::pipelineShutdown()
{
    try
    {
        m_pipeline.stop();
    }
    catch (const rs2::error& e)
    {
        yError() << "realsense2Driver: failed to stop the pipeline:"<< "(" << e.what() << ")";
        return false;
    }
    return true;
}

bool realsense2Driver::initializeRealsenseDevice()
{
    // TODO get configurations of the device, and read the value from the conf file
    double colorW = m_params_map[rgbRes].val[0].asDouble();
    double colorH = m_params_map[rgbRes].val[1].asDouble();
    double depthW = m_params_map[depthRes].val[0].asDouble();
    double depthH = m_params_map[depthRes].val[1].asDouble();
    m_cfg.enable_stream(RS2_STREAM_COLOR, colorW, colorH, RS2_FORMAT_RGB8, 0); //TODO Supported fps and res
    m_cfg.enable_stream(RS2_STREAM_DEPTH, depthW, depthH, RS2_FORMAT_Z16, 0);

    if (!pipelineStartup())
        return false;

    // Camera warmup - Dropped frames to allow stabilization
    yInfo()<<"realsense2Driver: sensor warm-up....";
    for (int i = 0; i < 30; i++)
    {
        m_pipeline.wait_for_frames();
    }
    yInfo()<<"realsense2Driver:....device ready!";
    // First, create a rs2::context.
    rs2::device_list devices = m_ctx.query_devices();

    rs2::device selected_device;
    if (devices.size() == 0)
    {
        yError() << "realsense2Driver: No device connected, please connect a RealSense device";

        rs2::device_hub device_hub(m_ctx);

        //Using the device_hub we can block the program until a device connects
        m_device = device_hub.wait_for_device();
    }
    else
    {
        //TODO: if more are connected?!
        // Update the selected device
        m_device = devices[0];
        if (m_verbose)
            yInfo()<<get_device_information(m_device).c_str();
    }


    // Given a device, we can query its sensors using:
    m_sensors = m_device.query_sensors();

    yInfo()<< "realsense2Driver: Device consists of" << m_sensors.size()<<"sensors";
    if (m_verbose)
    {
        for (size_t i=0; i < m_sensors.size(); i++)
        {
            print_supported_options(m_sensors[i]);
        }
    }

    for (size_t i=0; i < m_sensors.size(); i++)
    {
        if (m_sensors[i].is<rs2::depth_sensor>())
            m_depth_sensor = &m_sensors[i];
        else
            m_color_sensor = &m_sensors[i];
    }


    // Get stream intrinsics & extrinsics
    updateTransformations();
    return true;
}

void realsense2Driver::updateTransformations()
{
    rs2::pipeline_profile pipeline_profile = m_pipeline.get_active_profile();
    rs2::video_stream_profile depth_stream_profile = rs2::video_stream_profile(pipeline_profile.get_stream(RS2_STREAM_DEPTH));
    rs2::video_stream_profile color_stream_profile = rs2::video_stream_profile(pipeline_profile.get_stream(RS2_STREAM_COLOR));

    m_depth_intrin = depth_stream_profile.get_intrinsics();
    m_color_intrin = color_stream_profile.get_intrinsics();
    m_depth_to_color = depth_stream_profile.get_extrinsics_to(color_stream_profile);
    m_color_to_depth = color_stream_profile.get_extrinsics_to(depth_stream_profile);
}


void realsense2Driver::settingErrorMsg(const string& error, bool& ret)
{
    yError() << "realsense2Driver:" << error.c_str();
    ret = false;
}

bool realsense2Driver::setParams()
{
    bool ret = true;
    //ACCURACY
    if (m_params_map[accuracy].isSetting && ret)
    {
        if (!m_params_map[accuracy].val[0].isDouble() )
            settingErrorMsg("Param " + m_params_map[accuracy].name + " is not a double as it should be.", ret);

        if (! setDepthAccuracy(m_params_map[accuracy].val[0].asDouble() ) )
            settingErrorMsg("Setting param " + m_params_map[accuracy].name + " failed... quitting.", ret);
    }

    //CLIP_PLANES
    if (m_params_map[clipPlanes].isSetting && ret)
    {
        if (!m_params_map[clipPlanes].val[0].isDouble() )
            settingErrorMsg("Param " + m_params_map[clipPlanes].name + " is not a double as it should be.", ret);

        if (!m_params_map[clipPlanes].val[1].isDouble() )
            settingErrorMsg("Param " + m_params_map[clipPlanes].name + " is not a double as it should be.", ret);

        if (! setDepthClipPlanes(m_params_map[clipPlanes].val[0].asDouble(), m_params_map[clipPlanes].val[1].asDouble() ) )
            settingErrorMsg("Setting param " + m_params_map[clipPlanes].name + " failed... quitting.", ret);
    }

    //DEPTH_RES
    if (m_params_map[depthRes].isSetting && ret)
    {
        Value p1, p2;
        p1 = m_params_map[depthRes].val[0];
        p2 = m_params_map[depthRes].val[1];

        if (!p1.isInt() || !p2.isInt() )
        {
            settingErrorMsg("Param " + m_params_map[depthRes].name + " is not a int as it should be.", ret);
        }

        if (! setDepthResolution(p1.asInt(), p2.asInt()))
        {
            settingErrorMsg("Setting param " + m_params_map[depthRes].name + " failed... quitting.", ret);
        }
    }

    //RGB_RES
    if (m_params_map[rgbRes].isSetting && ret)
    {
        Value p1, p2;
        p1 = m_params_map[rgbRes].val[0];
        p2 = m_params_map[rgbRes].val[1];

        if (!p1.isInt() || !p2.isInt() )
        {
            settingErrorMsg("Param " + m_params_map[rgbRes].name + " is not a int as it should be.", ret);
        }

        if (! setRgbResolution(p1.asInt(), p2.asInt()))
        {
            settingErrorMsg("Setting param " + m_params_map[rgbRes].name + " failed... quitting.", ret);
        }
    }

    return ret;
}


bool realsense2Driver::open(Searchable& config)
{
    std::vector<RGBDSensorParamParser::RGBDParam*> params;
    for (auto& p:m_params_map)
    {
        params.push_back(&(p.second));
    }

    m_period = config.check("period", yarp::os::Value(30), "period of the camera").asInt();
    m_verbose = config.check("verbose");
    if (!m_paramParser->parseParam(config, params))
    {
        yError()<<"realsense2Driver: failed to parse the parameters";
        return false;
    }

    //"registered" is a hidden parameter for debugging pourpose
    m_depthRegistration = !(config.check("registered") && config.find("registered").isBool() && config.find("registered").asBool() == false);



    if (!initializeRealsenseDevice())
    {
        yError()<<"realsense2Driver: failed to initialize the realsense device";
        return false;
    }

    // setting Parameters
    if (!setParams())
    {
        return false;
    }

    return true;
}

bool realsense2Driver::close()
{
    pipelineShutdown();
    return true;
}

int realsense2Driver::getRgbHeight()
{
    return m_color_intrin.height;
}

int realsense2Driver::getRgbWidth()
{
    return m_color_intrin.width;
}

bool realsense2Driver::getRgbSupportedConfigurations(yarp::sig::VectorOf<CameraConfig> &configurations)
{
    yWarning()<<"realsense2Driver:getRgbSupportedConfigurations not implemented yet";
    return false;
}

bool realsense2Driver::getRgbResolution(int &width, int &height)
{
    width  = m_color_intrin.width;
    height = m_color_intrin.height;
    return true;
}

bool realsense2Driver::setDepthResolution(int width, int height)
{
    m_cfg.enable_stream(RS2_STREAM_COLOR, m_color_intrin.width, m_color_intrin.height, RS2_FORMAT_RGB8, 0); //TODO Supported fps and res
    m_cfg.enable_stream(RS2_STREAM_DEPTH, width, height, RS2_FORMAT_Z16, 0);

    if (!pipelineShutdown())
        return false;

    if (!pipelineStartup())
        return false;

    updateTransformations();
    return true;
}

bool realsense2Driver::setRgbResolution(int width, int height)
{
    m_cfg.enable_stream(RS2_STREAM_COLOR, width, height, RS2_FORMAT_RGB8, 0); //TODO Supported fps and res
    m_cfg.enable_stream(RS2_STREAM_DEPTH, m_depth_intrin.width, m_depth_intrin.height, RS2_FORMAT_Z16, 0);

    if (!pipelineShutdown())
        return false;

    if (!pipelineStartup())
        return false;

    updateTransformations();
    return true;
}


bool realsense2Driver::setRgbFOV(double horizontalFov, double verticalFov)
{
    // It seems to be not avilable...
    return false;
}

bool realsense2Driver::setDepthFOV(double horizontalFov, double verticalFov)
{
    // It seems to be not avilable...
    return false;
}

bool realsense2Driver::setDepthAccuracy(double accuracy)
{
    bool ret = true;
    ret = setOption(RS2_OPTION_ACCURACY, m_depth_sensor, accuracy);
    return ret;
}

bool realsense2Driver::getRgbFOV(double &horizontalFov, double &verticalFov)
{
    float fov[2];
    rs2_fov(&m_color_intrin, fov);
    horizontalFov = fov[0];
    verticalFov   = fov[1];
    return true;
}

bool realsense2Driver::getRgbMirroring(bool& mirror)
{
    yWarning()<<"realsense2Driver: mirroring not supported";
    return false;
}

bool realsense2Driver::setRgbMirroring(bool mirror)
{
    yWarning()<<"realsense2Driver: mirroring not supported";
    return false;
}

bool realsense2Driver::setIntrinsic(Property& intrinsic, const rs2_intrinsics &values)
{
    intrinsic.put("focalLengthX",       values.fx);
    intrinsic.put("focalLengthY",       values.fy);
    intrinsic.put("principalPointX",    values.ppx);
    intrinsic.put("principalPointY",    values.ppy);

    intrinsic.put("distortionModel", "plumb_bob");
    intrinsic.put("k1", values.coeffs[0]);
    intrinsic.put("k2", values.coeffs[1]);
    intrinsic.put("t1", values.coeffs[2]);
    intrinsic.put("t2", values.coeffs[3]);
    intrinsic.put("k3", values.coeffs[4]);

    intrinsic.put("stamp", yarp::os::Time::now());
    return true;
}

bool realsense2Driver::getRgbIntrinsicParam(Property& intrinsic)
{
    return setIntrinsic(intrinsic, m_color_intrin);
}

int  realsense2Driver::getDepthHeight()
{
    return m_depth_intrin.height;
}

int  realsense2Driver::getDepthWidth()
{
    return m_depth_intrin.width;
}

bool realsense2Driver::getDepthFOV(double& horizontalFov, double& verticalFov)
{
    float fov[2];
    rs2_fov(&m_depth_intrin, fov);
    horizontalFov = fov[0];
    verticalFov   = fov[1];
    return true;
}

bool realsense2Driver::getDepthIntrinsicParam(Property& intrinsic)
{
    return setIntrinsic(intrinsic, m_depth_intrin);;
}

double realsense2Driver::getDepthAccuracy()
{
    float accuracy = 0.0;
    if (getOption(RS2_OPTION_ACCURACY, m_depth_sensor, accuracy))
    {
        return accuracy;
    }
    return 0.0;
}

bool realsense2Driver::getDepthClipPlanes(double& nearPlane, double& farPlane)
{
    bool ret = true;
    ret  = getOption(RS2_OPTION_MIN_DISTANCE, m_depth_sensor, (float&) nearPlane);// Not sure it is the correct option
    ret &= getOption(RS2_OPTION_MAX_DISTANCE, m_depth_sensor, (float&) farPlane); // Not sure it is the correct option
    return ret;
}

bool realsense2Driver::setDepthClipPlanes(double nearPlane, double farPlane)
{
    bool ret = true;
    ret  = setOption(RS2_OPTION_MIN_DISTANCE, m_depth_sensor, nearPlane);// Not sure it is the correct option
    ret &= setOption(RS2_OPTION_MAX_DISTANCE, m_depth_sensor, farPlane); // Not sure it is the correct option
    return ret;
}

bool realsense2Driver::getDepthMirroring(bool& mirror)
{
    yWarning()<<"realsense2Driver: mirroring not supported";
    return false;
}

bool realsense2Driver::setDepthMirroring(bool mirror)
{
    yWarning()<<"realsense2Driver: mirroring not supported";
    return false;
}

bool realsense2Driver::getExtrinsicParam(Matrix& extrinsic)
{
    extrinsic = m_paramParser->transformationMatrix;
    return true;
}

bool realsense2Driver::getRgbImage(FlexImage& rgbImage, Stamp* timeStamp)
{
    rs2::frameset data = m_pipeline.wait_for_frames();
    return getImage(rgbImage, timeStamp, data);
}

bool realsense2Driver::getDepthImage(ImageOf<PixelFloat>& depthImage, Stamp* timeStamp)
{
    rs2::frameset data = m_pipeline.wait_for_frames();
    rs2::align align(RS2_STREAM_COLOR);
    auto aligned_frames = align.process(data);
    return getImage(depthImage, timeStamp, aligned_frames);
}

bool realsense2Driver::getImage(FlexImage& Frame, Stamp *timeStamp, rs2::frameset &sourceFrame)
{
    rs2::video_frame color_frm = sourceFrame.get_color_frame();
    rs2_format format = color_frm.get_profile().format();

    int pixCode = pixFormatToCode(format);
    size_t mem_to_wrt = color_frm.get_width() * color_frm.get_height() * bytesPerPixel(format);

    if (pixCode == VOCAB_PIXEL_INVALID)
    {
        yError() << "realsense2Driver: Pixel Format not recognized";
        return false;
    }

    Frame.setPixelCode(pixCode);
    Frame.resize(m_color_intrin.width, m_color_intrin.height);

    if ((size_t) Frame.getRawImageSize() != mem_to_wrt)
    {
        yError() << "realsense2Driver: device and local copy data size doesn't match";
        return false;
    }

    memcpy((void*)Frame.getRawImage(), (void*)color_frm.get_data(), mem_to_wrt);
    m_rgb_stamp.update();
    *timeStamp = m_rgb_stamp;
    return true;
}

bool realsense2Driver::getImage(depthImage& Frame, Stamp *timeStamp, const rs2::frameset &sourceFrame)
{
    rs2::depth_frame depth_frm = sourceFrame.get_depth_frame();
    rs2_format format = depth_frm.get_profile().format();

    int pixCode = pixFormatToCode(format);

    int w = depth_frm.get_width();
    int h = depth_frm.get_height();

    if (pixCode == VOCAB_PIXEL_INVALID)
    {
        yError() << "realsense2Driver: Pixel Format not recognized";
        return false;
    }

    Frame.resize(w, h);

    for (int x = 0; x < w; x++)
    {
        for (int y = 0; y < h ; y++)
        {
            Frame.safePixel(x, y) = depth_frm.get_distance(x, y);
        }
    }

    m_depth_stamp.update();
    *timeStamp   = m_depth_stamp;
    return true;
}

bool realsense2Driver::getImages(FlexImage& colorFrame, ImageOf<PixelFloat>& depthFrame, Stamp* colorStamp, Stamp* depthStamp)
{
    rs2::frameset data = m_pipeline.wait_for_frames();
    rs2::align align(RS2_STREAM_COLOR);
    auto aligned_frames = align.process(data);
    return getImage(colorFrame, colorStamp, aligned_frames) & getImage(depthFrame, depthStamp, aligned_frames);
}

IRGBDSensor::RGBDSensor_status realsense2Driver::getSensorStatus()
{
    return RGBD_SENSOR_OK_IN_USE;
}

ConstString realsense2Driver::getLastErrorMsg(Stamp* timeStamp)
{
    return "";
}

bool realsense2Driver::getCameraDescription(CameraDescriptor* camera)
{
    camera->deviceDescription = get_device_information(m_device);
    camera->busType = BUS_USB;
    return true;
}

bool realsense2Driver::hasFeature(int feature, bool* hasFeature)
{
    cameraFeature_id_t f;
    f = static_cast<cameraFeature_id_t>(feature);
    if (f < YARP_FEATURE_BRIGHTNESS || f > YARP_FEATURE_NUMBER_OF-1)
    {
        return false;
    }

    if (std::find(m_supportedFeatures.begin(), m_supportedFeatures.end(), f) != m_supportedFeatures.end())
    {
        *hasFeature = true;
    }
    else
    {
        *hasFeature = false;
    }

    return true;
}

bool realsense2Driver::setFeature(int feature, double value)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }

    cameraFeature_id_t f = static_cast<cameraFeature_id_t>(feature);
    switch(f)
    {
    case YARP_FEATURE_EXPOSURE:
        b = setOption(RS2_OPTION_EXPOSURE, m_color_sensor, value);
        return b;
    case YARP_FEATURE_GAIN:
        b = setOption(RS2_OPTION_GAIN, m_color_sensor, value);
        return b;
    case YARP_FEATURE_FRAME_RATE:
    {
        //TODO to implement
        return false;
    }
    case YARP_FEATURE_WHITE_BALANCE:
        b = setOption(RS2_OPTION_WHITE_BALANCE, m_color_sensor, value);
        return b;
    case YARP_FEATURE_SHARPNESS:
        b = setOption(RS2_OPTION_SHARPNESS, m_color_sensor, value);
        return b;
    case YARP_FEATURE_HUE:
        b = setOption(RS2_OPTION_HUE, m_color_sensor, value);
        return b;
    case YARP_FEATURE_SATURATION:
        b = setOption(RS2_OPTION_SATURATION, m_color_sensor, value);
        return b;
    default:
        yError() << "feature not supported!";
        return false;
    }
    return true;
}

bool realsense2Driver::getFeature(int feature, double *value)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }

    cameraFeature_id_t f = static_cast<cameraFeature_id_t>(feature);
    switch(f)
    {
    case YARP_FEATURE_EXPOSURE:
        b = getOption(RS2_OPTION_EXPOSURE, m_color_sensor, (float&) value);
        return b;
    case YARP_FEATURE_GAIN:
        b = getOption(RS2_OPTION_GAIN, m_color_sensor, (float&) value);
        return b;
    case YARP_FEATURE_FRAME_RATE:
    {
        //TODO to implement
        return false;
    }
    case YARP_FEATURE_WHITE_BALANCE:
        b = getOption(RS2_OPTION_WHITE_BALANCE, m_color_sensor, (float&) value);
        return b;
    case YARP_FEATURE_SHARPNESS:
        b = getOption(RS2_OPTION_SHARPNESS, m_color_sensor, (float&) value);
        return b;
    case YARP_FEATURE_HUE:
        b = getOption(RS2_OPTION_HUE, m_color_sensor, (float&) value);
        return b;
    case YARP_FEATURE_SATURATION:
        b = getOption(RS2_OPTION_SATURATION, m_color_sensor, (float&) value);
        return b;
    default:
        yError() << "feature not supported!";
        return false;
    }
    return true;
}

bool realsense2Driver::setFeature(int feature, double value1, double value2)
{
    yError() << "no 2-valued feature are supported";
    return false;
}

bool realsense2Driver::getFeature(int feature, double *value1, double *value2)
{
    yError() << "no 2-valued feature are supported";
    return false;
}

bool realsense2Driver::hasOnOff(  int feature, bool *HasOnOff)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }

    cameraFeature_id_t f = static_cast<cameraFeature_id_t>(feature);
    if (f == YARP_FEATURE_WHITE_BALANCE || f == YARP_FEATURE_MIRROR)
    {
        *HasOnOff = true;
        return true;
    }
    *HasOnOff = false;
    return true;
}

bool realsense2Driver::setActive( int feature, bool onoff)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }

    if (!hasOnOff(feature, &b) || !b)
    {
        yError() << "feature does not have OnOff.. call hasOnOff() to know if a specific feature support OnOff mode";
        return false;
    }

    switch(feature)
    {
    case YARP_FEATURE_WHITE_BALANCE:
        b = setOption(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE, m_color_sensor, (float) onoff); //TODO check if this exotic conversion works
        return b;
    case YARP_FEATURE_EXPOSURE:
        b = setOption(RS2_OPTION_ENABLE_AUTO_EXPOSURE, m_color_sensor, (float) onoff); //TODO check if this exotic conversion works
        return b;
    default:
        return false;
    }

    return true;
}

bool realsense2Driver::getActive( int feature, bool *isActive)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }

    if (!hasOnOff(feature, &b) || !b)
    {
        yError() << "feature does not have OnOff.. call hasOnOff() to know if a specific feature support OnOff mode";
        return false;
    }
    switch(feature)
    {
    case YARP_FEATURE_WHITE_BALANCE:
        b = getOption(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE, m_color_sensor, (float&) isActive); //TODO check if this exotic conversion works
        return b;
    case YARP_FEATURE_EXPOSURE:
        b = getOption(RS2_OPTION_ENABLE_AUTO_EXPOSURE, m_color_sensor, (float&) isActive); //TODO check if this exotic conversion works
        return b;
    default:
        return false;
    }

    return true;
}

bool realsense2Driver::hasAuto(int feature, bool *hasAuto)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }

    cameraFeature_id_t f = static_cast<cameraFeature_id_t>(feature);
    if (f == YARP_FEATURE_EXPOSURE || f == YARP_FEATURE_WHITE_BALANCE)
    {
        *hasAuto = true;
        return true;
    }
    *hasAuto = false;
    return true;
}

bool realsense2Driver::hasManual( int feature, bool* hasManual)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }

    cameraFeature_id_t f = static_cast<cameraFeature_id_t>(feature);
    if (f == YARP_FEATURE_EXPOSURE || f == YARP_FEATURE_FRAME_RATE || f == YARP_FEATURE_GAIN ||
        f == YARP_FEATURE_HUE || f == YARP_FEATURE_SATURATION || f == YARP_FEATURE_SHARPNESS)
    {
        *hasManual = true;
        return true;
    }
    *hasManual = false;
    return true;
}

bool realsense2Driver::hasOnePush(int feature, bool* hasOnePush)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }

    return hasAuto(feature, hasOnePush);
}

bool realsense2Driver::setMode(int feature, FeatureMode mode)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }
    float one = 1.0;
    float zero = 0.0;

    cameraFeature_id_t f = static_cast<cameraFeature_id_t>(feature);
    if (f == YARP_FEATURE_WHITE_BALANCE)
    {
        switch(mode)
        {
        case MODE_AUTO:
            return setOption(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE, m_color_sensor, one);
        case MODE_MANUAL:
            return setOption(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE, m_color_sensor, zero);
        case MODE_UNKNOWN:
            return false;
        default:
            return false;
        }
        return true;
    }

    if (f == YARP_FEATURE_EXPOSURE)
    {
        switch(mode)
        {
        case MODE_AUTO:
            return setOption(RS2_OPTION_ENABLE_AUTO_EXPOSURE, m_color_sensor, one);
        case MODE_MANUAL:
            return setOption(RS2_OPTION_ENABLE_AUTO_EXPOSURE, m_color_sensor, zero);
        case MODE_UNKNOWN:
            return false;
        default:
            return false;
        }
        return true;
    }


    yError() << "feature does not have both auto and manual mode";
    return false;
}

bool realsense2Driver::getMode(int feature, FeatureMode* mode)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }
    float res = 0.0;
    bool ret = true;
    cameraFeature_id_t f = static_cast<cameraFeature_id_t>(feature);
    if (f == YARP_FEATURE_WHITE_BALANCE)
    {
        ret &= getOption(RS2_OPTION_ENABLE_AUTO_WHITE_BALANCE, m_color_sensor, res);
    }

    if (f == YARP_FEATURE_EXPOSURE)
    {
        ret &= getOption(RS2_OPTION_ENABLE_AUTO_EXPOSURE, m_color_sensor, res);
    }

    if (res == 0.0)
    {
        *mode = MODE_MANUAL;
    }
    else if (res == 1.0)
    {
        *mode = MODE_AUTO;
    }
    else
    {
        *mode = MODE_UNKNOWN;
    }
    return ret;
}

bool realsense2Driver::setOnePush(int feature)
{
    bool b;
    if (!hasFeature(feature, &b) || !b)
    {
        yError() << "feature not supported!";
        return false;
    }

    if (!hasOnePush(feature, &b) || !b)
    {
        yError() << "feature doesn't have OnePush";
        return false;
    }

    setMode(feature, MODE_AUTO);
    setMode(feature, MODE_MANUAL);

    return true;
}