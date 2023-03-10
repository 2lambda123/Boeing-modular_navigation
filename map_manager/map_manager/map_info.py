import json
import rclpy.node

import datetime
from map_manager.msg import MapInfo as MapInfoMsg
from nav_msgs.msg import MapMetaData as MapMetaDataMsg
from geometry_msgs.msg import Pose as PoseMsg
from geometry_msgs.msg import Point as PointMsg


class MapInfo:
    def __init__(self,
                 name,
                 description,
                 created,
                 modified,
                 resolution,
                 width,
                 height,
                 origin_x,
                 origin_y
                 ) -> None:
        self.name: str = name
        self.description: str = description
        self.created: datetime.datetime = created
        self.modified: datetime.datetime = modified

        # Part of nav_msgs/MapMetaData
        self.resolution: float = resolution
        self.width: int = width
        self.height: int = height
        self.origin_x: float = origin_x
        self.origin_y: float = origin_y

    @classmethod
    def from_msg(cls, map_info_msg: MapInfoMsg):
        return MapInfo(
            name=map_info_msg.name,
            description=map_info_msg.description,
            created=datetime.datetime.utcfromtimestamp(map_info_msg.created.sec),
            modified=datetime.datetime.utcfromtimestamp(map_info_msg.modified.sec),
            resolution=map_info_msg.meta_data.resolution,
            width=map_info_msg.meta_data.width,
            height=map_info_msg.meta_data.height,
            origin_x=map_info_msg.meta_data.origin.position.x,
            origin_y=map_info_msg.meta_data.origin.position.y
        )

    @classmethod
    def from_dict(cls, dict: dict):
        return MapInfo(
            name=dict['name'],
            description=dict['description'],
            created=datetime.datetime.fromisoformat(dict['created']),
            modified=datetime.datetime.fromisoformat(dict['modified']),
            resolution=dict['meta_data']['resolution'],
            width=dict['meta_data']['width'],
            height=dict['meta_data']['height'],
            origin_x=dict['meta_data']['origin_x'],
            origin_y=dict['meta_data']['origin_y']
        )

    def to_msg(self, node: rclpy.node.Node):
        t = node.get_clock().now()
        return MapInfoMsg(
            name=self.name,
            description=self.description,
            created=t.to_msg(),
            modified=t.to_msg(),
            meta_data=MapMetaDataMsg(
                resolution=self.resolution,
                width=self.width,
                height=self.height,
                origin=PoseMsg(position=PointMsg(x=self.origin_x, y=self.origin_y))
            )
        )

    def to_simple_dict(self):
        return {
            'name': self.name,
            'description': self.description,
            'created': self.created.isoformat(),
            'modified': self.modified.isoformat(),
            'meta_data': {
                'resolution': self.resolution,
                'width': self.width,
                'height': self.height,
                'origin_x': self.origin_x,
                'origin_y': self.origin_y
            }
        }

    def to_json(self, file):
        with open(file, 'w') as fp:
            json.dump(self.to_simple_dict, fp, indent=4)

    def to_jsons(self, file):
        with open(file, 'w') as fp:
            json_str = json.dumps(self.to_simple_dict, fp)

        return json_str
