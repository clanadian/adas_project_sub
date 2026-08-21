from setuptools import find_packages, setup

package_name = 'rpi_adas_demo'

setup(
    name=package_name,
    version='0.1.0',
    packages=find_packages(exclude=['test']),
    data_files=[
        ('share/ament_index/resource_index/packages', ['resource/rpi_adas_demo']),
        ('share/' + package_name, ['package.xml']),
        ('share/' + package_name + '/launch', ['launch/demo.launch.py']),
        ('share/' + package_name + '/config', ['config/mediamtx.yml']),
        ('share/' + package_name + '/web', ['web/index.html']),
    ],
    install_requires=['setuptools'],
    zip_safe=True,
    maintainer='todo',
    maintainer_email='todo@example.com',
    description='RPi-local ADAS safety demo (KR260 development stand-in)',
    license='Apache-2.0',
    tests_require=['pytest'],
    entry_points={
        'console_scripts': [
            'yolo_safety_node = rpi_adas_demo.yolo_safety_node:main',
            'cmd_vel_arbiter = rpi_adas_demo.cmd_vel_arbiter:main',
            'uart_safety_receiver = rpi_adas_demo.uart_safety_receiver:main',
            'webrtc_bridge = rpi_adas_demo.webrtc_bridge_node:main',
        ],
    },
)
