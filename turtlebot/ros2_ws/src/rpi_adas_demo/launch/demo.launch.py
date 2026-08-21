"""RPi ADAS demo: joystick, ONNX YOLO safety, arbiter, and MediaMTX WebRTC."""

import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, ExecuteProcess
from launch.conditions import IfCondition
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue


_PACKAGE = get_package_share_directory('rpi_adas_demo')
_MODELS = '/home/ubuntu/ros2_ws/src/rpi_adas_demo/models'
_DEFAULT_MODEL = os.path.join(
    _MODELS, 'yolov3-tiny-adas-5class-512x288-int8-qdq.onnx')
_DEFAULT_NAMES = os.path.join(_MODELS, 'adas.names')
_DEFAULT_MEDIAMTX = os.path.expanduser('~/mediamtx')
_DEFAULT_MEDIAMTX_CONFIG = os.path.join(_PACKAGE, 'config', 'mediamtx.yml')


def generate_launch_description() -> LaunchDescription:
    arguments = [
        DeclareLaunchArgument(
            'model_path', default_value=_DEFAULT_MODEL,
            description='Converted YOLOv3-tiny ONNX model'),
        DeclareLaunchArgument(
            'names_path', default_value=_DEFAULT_NAMES,
            description='One class name per line'),
        DeclareLaunchArgument(
            'camera_index', default_value='0',
            description='V4L2 USB camera index'),
        DeclareLaunchArgument(
            'cam_width', default_value='1280',
            description='Annotated stream capture width'),
        DeclareLaunchArgument(
            'cam_height', default_value='720',
            description='Annotated stream capture height'),
        DeclareLaunchArgument(
            'publish_fps', default_value='10.0',
            description='Maximum annotated stream frame rate'),
        DeclareLaunchArgument(
            'debug_jpeg_quality', default_value='80',
            description='Intermediate ROS JPEG quality (1-100)'),
        DeclareLaunchArgument(
            'stream_bitrate_kbps', default_value='3000',
            description='WebRTC H.264 bitrate in kbit/s'),
        DeclareLaunchArgument(
            'gstreamer_encoder', default_value='x264enc',
            description='x264enc or v4l2h264enc'),
        DeclareLaunchArgument(
            'enable_webrtc', default_value='true',
            description='Start MediaMTX and the ROS-to-RTSP bridge'),
        DeclareLaunchArgument(
            'mediamtx_path', default_value=_DEFAULT_MEDIAMTX,
            description='MediaMTX arm64 executable'),
        DeclareLaunchArgument(
            'mediamtx_config', default_value=_DEFAULT_MEDIAMTX_CONFIG,
            description='MediaMTX YAML configuration'),
        DeclareLaunchArgument(
            'no_joy_mode', default_value='false',
            description='Use /cmd_vel_manual instead of Xbox /joy'),
    ]

    mediamtx = ExecuteProcess(
        cmd=[LaunchConfiguration('mediamtx_path'),
             LaunchConfiguration('mediamtx_config')],
        name='mediamtx',
        output='screen',
        condition=IfCondition(LaunchConfiguration('enable_webrtc')),
    )

    joy = Node(
        package='joy',
        executable='joy_node',
        name='joy_node',
        parameters=[{'deadzone': 0.05, 'autorepeat_rate': 0.0}],
    )

    yolo = Node(
        package='rpi_adas_demo',
        executable='yolo_safety_node',
        name='yolo_safety_node',
        parameters=[{
            'model_path': LaunchConfiguration('model_path'),
            'names_path': LaunchConfiguration('names_path'),
            'camera_index': ParameterValue(
                LaunchConfiguration('camera_index'), value_type=int),
            'cam_width': ParameterValue(
                LaunchConfiguration('cam_width'), value_type=int),
            'cam_height': ParameterValue(
                LaunchConfiguration('cam_height'), value_type=int),
            'camera_fourcc': 'MJPG',
            'camera_fps': 30.0,
            'conf_threshold': 0.35,
            'nms_threshold': 0.4,
            'inference_threads': 3,
            'use_roi': False,
            'stop_duration': 3.0,
            'miss_frames': 10,
            'publish_debug_image': True,
            'publish_fps': ParameterValue(
                LaunchConfiguration('publish_fps'), value_type=float),
            'debug_jpeg_quality': ParameterValue(
                LaunchConfiguration('debug_jpeg_quality'), value_type=int),
        }],
    )

    webrtc_bridge = Node(
        package='rpi_adas_demo',
        executable='webrtc_bridge',
        name='webrtc_bridge',
        condition=IfCondition(LaunchConfiguration('enable_webrtc')),
        parameters=[{
            'topic': '/adas/debug_image/compressed',
            'rtsp_url': 'rtsp://127.0.0.1:8554/adas',
            'stream_fps': ParameterValue(
                LaunchConfiguration('publish_fps'), value_type=float),
            'bitrate_kbps': ParameterValue(
                LaunchConfiguration('stream_bitrate_kbps'), value_type=int),
            'gstreamer_encoder': LaunchConfiguration('gstreamer_encoder'),
        }],
    )

    arbiter = Node(
        package='rpi_adas_demo',
        executable='cmd_vel_arbiter',
        name='cmd_vel_arbiter',
        parameters=[{
            'safety_timeout_sec': 2.0,
            'joy_timeout_sec': 1.0,
            'publish_hz': 20.0,
            'slow_linear_max': 0.10,
            'slow_angular_max': 0.80,
            'linear_axis_sign': -1.0,
            'stick_deadzone': 0.12,
            'cardinal_snap_ratio': 0.45,
            'no_joy_mode': LaunchConfiguration('no_joy_mode'),
        }],
    )

    return LaunchDescription(
        arguments + [mediamtx, joy, yolo, webrtc_bridge, arbiter])
