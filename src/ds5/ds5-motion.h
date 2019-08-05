// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#pragma once

#include "ds5-device.h"

namespace librealsense
{
    // Enforce complile-time verification of all the assigned FPS profiles
    enum class IMU_OUTPUT_DATA_RATES : uint16_t
    {
        IMU_FPS_63  = 63,
        IMU_FPS_100 = 100,
        IMU_FPS_200 = 200,
        IMU_FPS_250 = 250,
        IMU_FPS_400 = 400
    };

    using odr = IMU_OUTPUT_DATA_RATES;

#ifdef _WIN32
    static const std::string gyro_sensor_name = "HID Sensor Class Device: Gyroscope";
    static const std::string accel_sensor_name = "HID Sensor Class Device: Accelerometer";
    static const std::map<odr, uint16_t> hid_fps_translation =
    {  //FPS   Value to send to the Driver
        {odr::IMU_FPS_63,   1000},
        {odr::IMU_FPS_100,  1000},
        {odr::IMU_FPS_200,  500},
        {odr::IMU_FPS_250,  400},
        {odr::IMU_FPS_400,  250} };

#else
    static const std::string gyro_sensor_name = "gyro_3d";
    static const std::string accel_sensor_name = "accel_3d";
    static const std::map<IMU_OUTPUT_DATA_RATES, unsigned> hid_fps_translation =
    {  //FPS   Value to send to the Driver
        {IMU_FPS_63,   1},
        {IMU_FPS_100,  1},
        {IMU_FPS_200,  2},
        {IMU_FPS_250,  3},
        {IMU_FPS_400,  4} };
#endif

    class mm_calib_parser
    {
    public:
        virtual rs2_extrinsics get_extrinsic_to(rs2_stream) = 0;    // Extrinsics are referenced to the Depth stream, except for TM1
        virtual ds::imu_intrinsic get_intrinsic(rs2_stream) = 0;    // With extrinsic from FE<->IMU only
        virtual float3x3 imu_to_depth_alignment() = 0;
    };

    class tm1_imu_calib_parser : public mm_calib_parser
    {
    public:
        tm1_imu_calib_parser(const std::vector<uint8_t>& raw_data)
        {
            calib_table = *(ds::check_calib<ds::tm1_eeprom>(raw_data));
        };
        tm1_imu_calib_parser(const tm1_imu_calib_parser&);
        ~tm1_imu_calib_parser(){};

        float3x3 imu_to_depth_alignment() { return {{1,0,0},{0,1,0},{0,0,1}}; }

        rs2_extrinsics get_extrinsic_to(rs2_stream stream)
        {
            if (!(RS2_STREAM_ACCEL == stream) && !(RS2_STREAM_GYRO == stream) && !(RS2_STREAM_FISHEYE == stream))
                throw std::runtime_error(to_string() << "TM1 Calibration does not provide extrinsic for : " << rs2_stream_to_string(stream) << " !");

            auto fe_calib = calib_table.calibration_table.calib_model.fe_calibration;

            auto rot = fe_calib.fisheye_to_imu.rotation;
            auto trans = fe_calib.fisheye_to_imu.translation;

            pose ex = { { rot(0,0), rot(1,0),rot(2,0),rot(0,1), rot(1,1),rot(2,1),rot(0,2), rot(1,2),rot(2,2) },
            { trans[0], trans[1], trans[2] } };

            if (RS2_STREAM_FISHEYE == stream)
                return inverse(from_pose(ex));
            else
                return from_pose(ex);
        };

        ds::imu_intrinsic get_intrinsic(rs2_stream stream)
        {
            ds::imu_intrinsics in_intr;
            switch (stream)
            {
            case RS2_STREAM_ACCEL:
                in_intr = calib_table.calibration_table.imu_calib_table.accel_intrinsics; break;
            case RS2_STREAM_GYRO:
                in_intr = calib_table.calibration_table.imu_calib_table.gyro_intrinsics; break;
            default:
                throw std::runtime_error(to_string() << "TM1 IMU Calibration does not support intrinsic for : " << rs2_stream_to_string(stream) << " !");
            }
            ds::imu_intrinsic out_intr{};
            for (auto i = 0; i < 3; i++)
            {
                out_intr.sensitivity(i,i)   = in_intr.scale[i];
                out_intr.bias[i]            = in_intr.bias[i];
            }
            return out_intr;
        }

    private:
        ds::tm1_eeprom  calib_table;
    };

    class dm_v2_imu_calib_parser : public mm_calib_parser
    {
    public:
        dm_v2_imu_calib_parser(const std::vector<uint8_t>& raw_data, bool valid = true)
        {
            calib_table.module_info.dm_v2_calib_table.extrinsic_valid = 0;
            calib_table.module_info.dm_v2_calib_table.intrinsic_valid = 0;
            // default parser to be applied when no FW calibration is available
            if (valid)
                calib_table = *(ds::check_calib<ds::dm_v2_eeprom>(raw_data));
        };
        dm_v2_imu_calib_parser(const dm_v2_imu_calib_parser&);
        ~dm_v2_imu_calib_parser() {};

        float3x3 imu_to_depth_alignment() { return {{-1,0,0},{0,1,0},{0,0,-1}}; } //Reference spec : Bosch BMI055

        rs2_extrinsics get_extrinsic_to(rs2_stream stream)
        {
            if (!(RS2_STREAM_ACCEL == stream) && !(RS2_STREAM_GYRO == stream))
                throw std::runtime_error(to_string() << "Depth Module V2 does not support extrinsic for : " << rs2_stream_to_string(stream) << " !");

            rs2_extrinsics extr;
            if (1 == calib_table.module_info.dm_v2_calib_table.extrinsic_valid)
            {
                // The extrinsic is stored as array of floats / little-endian
                librealsense::copy(&extr, &calib_table.module_info.dm_v2_calib_table.depth_to_imu, sizeof(rs2_extrinsics));
            }
            else
            {
                LOG_INFO("IMU extrinsic table not found; using CAD values");
                // D435i specific - BMI055 assembly transformation based on mechanical drawing (mm)
                extr = { { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, { -0.00552f, 0.0051f, 0.01174f} };
            }
            return extr;
        }

        ds::imu_intrinsic get_intrinsic(rs2_stream stream)
        {
            if (1!=calib_table.module_info.dm_v2_calib_table.intrinsic_valid)
                throw std::runtime_error(to_string() << "Depth Module V2 intrinsic invalidated : " << rs2_stream_to_string(stream) << " !");

            ds::dm_v2_imu_intrinsic in_intr;
            switch (stream)
            {
                case RS2_STREAM_ACCEL:
                    in_intr = calib_table.module_info.dm_v2_calib_table.accel_intrinsic;
                    break;
                case RS2_STREAM_GYRO:
                    in_intr = calib_table.module_info.dm_v2_calib_table.gyro_intrinsic;
                    in_intr.bias = in_intr.bias * static_cast<float>(d2r);        // The gyro bias is calculated in Deg/sec
                    break;
                default:
                    throw std::runtime_error(to_string() << "Depth Module V2 does not provide intrinsic for stream type : " << rs2_stream_to_string(stream) << " !");
            }

            return { in_intr.sensitivity, in_intr.bias, {0,0,0}, {0,0,0} };
        }

    private:
        ds::dm_v2_eeprom  calib_table;
    };

    class mm_calib_handler
    {
    public:
        mm_calib_handler(std::shared_ptr<hw_monitor> hw_monitor);
        mm_calib_handler(const mm_calib_handler&);
        ~mm_calib_handler() {}

        ds::imu_intrinsic get_intrinsic(rs2_stream);
        rs2_extrinsics get_extrinsic(rs2_stream);       // The extrinsic defined as Depth->Stream rigid-body transfom.
        const std::vector<uint8_t> get_fisheye_calib_raw();
        float3x3 imu_to_depth_alignment() { return (*_calib_parser)->imu_to_depth_alignment(); }

    private:
        std::shared_ptr<hw_monitor> _hw_monitor;
        lazy< std::shared_ptr<mm_calib_parser>> _calib_parser;
        lazy<std::vector<uint8_t>>      _imu_eeprom_raw;
        std::vector<uint8_t>            get_imu_eeprom_raw() const;
        lazy<std::vector<uint8_t>>      _fisheye_calibration_table_raw;
    };

    class ds5_motion : public virtual ds5_device
    {
    public:
        std::shared_ptr<hid_sensor> create_hid_device(std::shared_ptr<context> ctx,
                                                      const std::vector<platform::hid_device_info>& all_hid_infos,
                                                      const firmware_version& camera_fw_version);

        ds5_motion(std::shared_ptr<context> ctx,
                   const platform::backend_device_group& group);

        rs2_motion_device_intrinsic get_motion_intrinsics(rs2_stream) const;

        std::shared_ptr<auto_exposure_mechanism> register_auto_exposure_options(uvc_sensor* uvc_ep,
                                                                                const platform::extension_unit* fisheye_xu);

    private:

        friend class ds5_fisheye_sensor;
        friend class ds5_hid_sensor;

        void initialize_fisheye_sensor(std::shared_ptr<context> ctx, const platform::backend_device_group& group);

        optional_value<uint8_t> _fisheye_device_idx;
        optional_value<uint8_t> _motion_module_device_idx;

        std::shared_ptr<mm_calib_handler>        _mm_calib;
        lazy<ds::imu_intrinsic>                 _accel_intrinsic;
        lazy<ds::imu_intrinsic>                 _gyro_intrinsic;
        lazy<std::vector<uint8_t>>              _fisheye_calibration_table_raw;
        std::shared_ptr<lazy<rs2_extrinsics>>   _depth_to_imu;                  // Mechanical installation pose

        // Bandwidth parameters required for HID sensors
        // The Acceleration configuration will be resolved according to the IMU sensor type at run-time
        std::vector<std::pair<std::string, stream_profile>> sensor_name_and_hid_profiles =
        { { gyro_sensor_name,     {RS2_STREAM_GYRO,  0, 1, 1, int(odr::IMU_FPS_200), RS2_FORMAT_MOTION_XYZ32F}},
          { gyro_sensor_name,     {RS2_STREAM_GYRO,  0, 1, 1, int(odr::IMU_FPS_400), RS2_FORMAT_MOTION_XYZ32F}}};

        // Translate frequency to SENSOR_PROPERTY_CURRENT_REPORT_INTERVAL.
        std::map<rs2_stream, std::map<unsigned, unsigned>> fps_and_sampling_frequency_per_rs2_stream =
        { { RS2_STREAM_GYRO,     {{unsigned(odr::IMU_FPS_200),  hid_fps_translation.at(odr::IMU_FPS_200)},
                                 { unsigned(odr::IMU_FPS_400),  hid_fps_translation.at(odr::IMU_FPS_400)}}} };

    protected:
        std::shared_ptr<stream_interface> _fisheye_stream;
        std::shared_ptr<stream_interface> _accel_stream;
        std::shared_ptr<stream_interface> _gyro_stream;
    };
}
