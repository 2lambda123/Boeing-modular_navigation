#!/usr/bin/env python3

# This script uploads all the map components in a folder to the map database
# Components:
# MapInfo (JSON)
# Occupancy Grid (PNG)
# Pbstream (pbstream)
# Node Graph (JSON)
# Area Tree (JSON)
# Zones (JSON)

from datetime import datetime
import json
import logging
from PIL import Image
import mongoengine
import os
import io
import threading

import rclpy
import rclpy.executors
from rclpy.parameter import Parameter

from map_manager.documents import Map as MapDoc
from map_manager.documents import Point as PointDoc
from map_manager.documents import Pose as PoseDoc
from map_manager.documents import Quaternion as QuaternionDoc
from map_manager.documents import Zone as ZoneDoc
from map_manager.map_info import MapInfo as MapInfoCls

from graph_map.area import Zone
from graph_map.node_graph_manager import NodeGraphManager
from graph_map.area_manager import AreaManager
from map_manager.srv import AddMap
from sensor_msgs.msg import CompressedImage

from map_manager.config import DATABASE_NAME

logger = logging.getLogger(__name__)


def dir_path(string):
    if string is None:
        return None
    else:
        if os.path.isdir(string):
            return string
        else:
            raise NotADirectoryError(string)


def run(
        node,
        dir,
        node_name,
        mongo_hostname='localhost',
        mongo_port=27017):
    # Map Info
    if os.path.exists(os.path.join(dir, 'map_info.json')):
        with open(os.path.join(dir, 'map_info.json'), 'r') as fp:
            map_info = MapInfoCls.from_dict(json.load(fp))
            logger.info('Found "map_info.json" with map name: {}'.format(map_info.name))
    else:
        logger.error('Mandatory file "map_info.json" not found. Quitting.')
        exit(1)

    # Occupancy Grid
    if os.path.exists(os.path.join(dir, 'occupancy_grid.png')):
        with open(os.path.join(dir, 'occupancy_grid.png'), 'rb') as fp:
            og_bytes = fp.read()
    else:
        og_bytes = None
        logger.error('"occupancy_grid.png" not found')

    # Pbstream
    if os.path.exists(os.path.join(dir, 'cartographer_map.pbstream')):
        with open(os.path.join(dir, 'cartographer_map.pbstream'), 'rb') as fp:
            pb_bytes = fp.read()
    else:
        pb_bytes = None
        logger.warning('"cartographer_map.pbstream" not found')

    # Node graph
    if os.path.exists(os.path.join(dir, 'node_graph.json')):
        # We could just read as a string and upload as-is but this way we can verify we have a valid JSON
        gm = NodeGraphManager.read_graph(os.path.join(dir, 'node_graph.json'))
        logger.info('Found "node_graph.json" with {} nodes'.format(len(gm.get_nodes().keys())))
    else:
        gm = None
        logger.warning('"node_graph.json" not found')

    # Area Tree
    if os.path.exists(os.path.join(dir, 'area_tree.json')):
        # We could just read as a string and upload as-is but this way we can verify we have a valid JSON
        am = AreaManager.read_graph(os.path.join(dir, 'area_tree.json'))
        logger.info('Found "area_tree.json" with {} areas'.format(len(am.get_areas(level=1).keys())))
    else:
        am = None
        logger.warning('"area_tree.json" not found')

    # Zones
    if os.path.exists(os.path.join(dir, 'zones.json')):
        with open(os.path.join(dir, 'zones.json'), 'r') as fp:
            zone_dicts = json.load(fp)
            zones = [Zone.from_dict(zone_dict) for zone_dict in zone_dicts]
        logger.info('Found "zones.json" with {} zones'.format(len(zones)))
    else:
        zones = None
        logger.warning('"zones.json" not found')

    #
    # Save the map via ROS interface
    #
    if node_name and (node is not None):
        logger.info('Uploading map via ROS to {}'.format(node_name))

        if gm is not None:
            gm_str = gm.to_json()
        else:
            gm_str = ''
        if am is not None:
            am_str = am.to_json()
        else:
            am_str = ''

        if zones is None:
            zones = list()

        add_map_client = node.create_client(
            AddMap,
            node_name + '/add_map'
        )

        logger.info('Waiting for AddMap service...')
        add_map_client.wait_for_service(timeout_sec=5.0)

        add_map_future = add_map_client.call_async(
            AddMap.Request(
                map_info=map_info.to_msg(node),
                node_graph=gm_str,
                area_tree=am_str,
                zones=[zone.to_msg() for zone in zones],
                occupancy_grid=CompressedImage(
                    format='png',
                    data=og_bytes
                ),
                pbstream=pb_bytes
            )
        )  # type: AddMap.Response

        logger.info('Calling AddMap service...')  # DEBUG

        rclpy.spin_until_future_complete(node, add_map_future)
        add_map_res: AddMap.Response = add_map_future.result()

        if not add_map_res.success:
            raise Exception(
                'Failed to save map: {}'.format(add_map_res.message))

    #
    # Save to mongo database without ROS
    #
    else:
        logger.info('Uploading map directly to database')
        logger.info('Connecting to {}:{}'.format(mongo_hostname, mongo_port))
        mongoengine.connect(
            db=DATABASE_NAME, host=mongo_hostname, port=mongo_port)

        map_query = MapDoc.objects(name=map_info.name)
        if map_query.count():
            map_obj = map_query.first()
            logger.info('Map {} already exists in the database. Updating.'.format(map_info.name))
        else:
            map_obj = MapDoc()

        map_obj.name = map_info.name
        map_obj.description = map_info.description
        map_obj.modified = datetime.utcnow()
        map_obj.width = map_info.width  # width and height are stored as number of cells
        map_obj.height = map_info.height
        map_obj.resolution = map_info.resolution
        map_obj.origin = PoseDoc(
            position=PointDoc(
                x=map_info.origin_x,
                y=map_info.origin_y,
                z=0),
            quaternion=QuaternionDoc(
                x=0,
                y=0,
                z=0,
                w=1.0
            )
        )

        # Upload occupancy grid as PNG
        if og_bytes is not None:
            map_obj.image.replace(io.BytesIO(og_bytes))

            map_obj.thumbnail.delete()
            map_obj.thumbnail.new_file()
            im = Image.open(io.BytesIO(og_bytes))
            im.thumbnail(size=(400, 400))
            im.save(fp=map_obj.thumbnail, format='PPM')
            map_obj.thumbnail.close()

        # Upload pbstream
        if pb_bytes is not None:
            map_obj.pbstream.replace(io.BytesIO(pb_bytes))

        # Graph maps
        if gm is not None:
            map_obj.node_graph = gm.to_json()
        if am is not None:
            map_obj.area_tree = am.to_json()

        # Zones
        if zones is not None:
            map_obj.zones.clear()
            for zone in zones:
                map_obj.zones.append(ZoneDoc.from_msg(zone.to_msg()))

        map_obj.save()

    logger.info('Done!')


def upload_map(args=None):
    try:
        rclpy.init(args=args)

        node = rclpy.create_node("upload_map")

        executor = rclpy.executors.MultiThreadedExecutor()
        executor.add_node(node)

        # Spin nodes in a separate thread
        executor_thread = threading.Thread(target=executor.spin, daemon=True)
        executor_thread.start()

        # Get node parameters
        node.mongo_hostname = node.declare_parameter(
            '~mongo_hostname', 'localhost').value
        mongo_hostname = node.get_parameter('~mongo_hostname').value

        node.mongo_port = node.declare_parameter('~mongo_port', 27017).value
        mongo_port = node.get_parameter('~mongo_port').value

        node.declare_parameter('~map_dir', Parameter.Type.STRING)
        map_dir = node.get_parameter(
            '~map_dir').value  # path string

        node.declare_parameter('~node_name', 'map_manager')
        node_name = node.get_parameter(
            '~node_name').value  # string

        run(
            node=node,
            dir=map_dir,
            node_name=node_name,
            mongo_hostname=mongo_hostname,
            mongo_port=mongo_port
        )

        node.destroy_node()

        executor.shutdown()
        executor_thread.join()

    except Exception:
        rclpy.shutdown()
        raise


if __name__ == '__main__':
    upload_map()