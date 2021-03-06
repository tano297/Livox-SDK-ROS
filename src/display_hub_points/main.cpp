//
// The MIT License (MIT)
//
// Copyright (c) 2019 Livox. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <string.h>


#include <apr_time.h>

#include "livox_sdk.h"
#include <ros/ros.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/point_cloud2_iterator.h>


#define BUFFER_POINTS                   (128*1024) // must be 2^n
#define POINTS_PER_FRAME                5000      // must < BUFFER_POINTS
#define PACKET_GAP_MISS_TIME            (1500000) // 1.5ms

struct PointCloudQueue {
  LivoxPoint buffer[BUFFER_POINTS];
  volatile uint32_t rd_idx;
  volatile uint32_t wr_idx;
  uint32_t mask;
  uint32_t size;  // must be 2^n
};

typedef struct {
  uint32_t receive_packet_count;
  uint32_t loss_packet_count;
  uint64_t last_timestamp;
} LidarPacketStatistic;

PointCloudQueue point_cloud_queue_pool[kMaxLidarCount];
/* for global publisher use */
ros::Publisher cloud_pub;


/* for device connect use ----------------------------------------------------------------------- */
typedef enum {
  kDeviceStateDisconnect = 0,
  kDeviceStateConnect = 1,
  kDeviceStateSampling = 2,
} DeviceState;

typedef struct {
  uint8_t handle;
  DeviceState device_state;
  DeviceInfo info;
  LidarPacketStatistic statistic_info;
} DeviceItem;

DeviceItem lidars[kMaxLidarCount];
DeviceItem hub;


/* user add hub broadcast code here */
const char* broadcast_code_list[] = {
  "13UUFBN003000B0"
};

#define BROADCAST_CODE_LIST_SIZE    (sizeof(broadcast_code_list) / sizeof(intptr_t))


/* for pointcloud queue process */
void PointCloudPoolInit(void) {
  for (int i=0; i<kMaxLidarCount; i++) {
    point_cloud_queue_pool[i].rd_idx = 0;
    point_cloud_queue_pool[i].wr_idx = 0;
    point_cloud_queue_pool[i].size = BUFFER_POINTS;
    point_cloud_queue_pool[i].mask = BUFFER_POINTS - 1;
  }
}

uint32_t QueuePop(PointCloudQueue* queue, LivoxPoint* out_point) {
  uint32_t mask = queue->mask;
  uint32_t rd_idx = queue->rd_idx & mask;

  out_point->x = queue->buffer[rd_idx].x;
  out_point->y = queue->buffer[rd_idx].y;
  out_point->z = queue->buffer[rd_idx].z;
  out_point->reflectivity = queue->buffer[rd_idx].reflectivity;

  queue->rd_idx++;
}

uint32_t QueuePush(PointCloudQueue *queue, LivoxPoint * in_point) {
  uint32_t mask = queue->mask;
  uint32_t wr_idx = queue->wr_idx & mask;

  queue->buffer[wr_idx].x = in_point->x;
  queue->buffer[wr_idx].y = in_point->y;
  queue->buffer[wr_idx].z = in_point->z;
  queue->buffer[wr_idx].reflectivity = in_point->reflectivity;

  queue->wr_idx++;
}

uint32_t QueueUsedSize(PointCloudQueue *queue) {
  return (queue->wr_idx - queue->rd_idx) & queue->mask;
}

uint32_t QueueIsFull(PointCloudQueue *queue) {
  return ((queue->wr_idx + 1) == queue->rd_idx);
}

uint32_t QueueIsEmpty(PointCloudQueue *queue) {
  return (queue->rd_idx == queue->wr_idx);
}

/* for pointcloud convert process */
static uint32_t PublishPointcloudData(PointCloudQueue *queue, uint32_t num) {
  /* init point cloud data struct */
  sensor_msgs::PointCloud cloud;

  cloud.header.stamp = ros::Time::now();
  cloud.header.frame_id = "sensor_frame";

  cloud.points.resize(num);
  cloud.channels.resize(1);
  cloud.channels[0].name = "rgb";
  cloud.channels[0].values.resize(num);

  LivoxPoint points;

  for (unsigned int i = 0; i < num; i++) {
    QueuePop(queue, &points);
    cloud.points[i].x = points.x;
    cloud.points[i].y = points.y;
    cloud.points[i].z = points.z;
    cloud.channels[0].values[i] = points.reflectivity;
  }

  cloud_pub.publish(cloud);
}

static void PointCloudConvert(LivoxPoint *p_dpoint, LivoxRawPoint *p_raw_point) {
  p_dpoint->x = p_raw_point->x/1000.0f;
  p_dpoint->y = p_raw_point->y/1000.0f;
  p_dpoint->z = p_raw_point->z/1000.0f;
  p_dpoint->reflectivity = p_raw_point->reflectivity;
}

void GetLidarData(uint8_t hub_handle, LivoxEthPacket *data, uint32_t data_num) {

  LivoxEthPacket *lidar_pack = data;

  if (!data || !data_num) {
    return;
  }

  /* caculate which lidar this eth packet data belong to */
  uint8_t handle = HubGetLidarHandle(lidar_pack->slot, lidar_pack->id);
  
  if ((hub_handle >= kMaxLidarCount) || (handle >= kMaxLidarCount)) {
    return;
  }

  if ((lidar_pack->timestamp_type == kTimestampTypeNoSync) || \
      (lidar_pack->timestamp_type == kTimestampTypePtp) ||\
      (lidar_pack->timestamp_type == kTimestampTypePps)) {
    LidarPacketStatistic *packet_statistic = &lidars[handle].statistic_info;
    uint64_t cur_timestamp = *((uint64_t *)(lidar_pack->timestamp));
    int64_t packet_gap    = cur_timestamp - packet_statistic->last_timestamp;

    packet_statistic->receive_packet_count++;
    if (packet_statistic->last_timestamp) {
      if (packet_gap > PACKET_GAP_MISS_TIME) { //lidars[handle].info.broadcast_code, \
        packet_statistic->loss_packet_count++;
        printf("%d miss count : %ld %lu %lu %d\r\n", \
               handle, packet_gap,\
               cur_timestamp, packet_statistic->last_timestamp,\
               packet_statistic->loss_packet_count);
      }
    }

    packet_statistic->last_timestamp = cur_timestamp;
  }

  LivoxRawPoint *p_point_data = (LivoxRawPoint *)lidar_pack->data;
  PointCloudQueue *p_queue    = &point_cloud_queue_pool[handle];
  LivoxPoint tmp_point;
  while (data_num) {
    PointCloudConvert(&tmp_point, p_point_data);
    if (QueueIsFull(p_queue)) {
      break;
    }

    QueuePush(p_queue, &tmp_point);
    --data_num;
    p_point_data++;
  }

  return;
}

void PollPointcloudData(void) {
  for (int i = 0; i < kMaxLidarCount; i++) {
    PointCloudQueue *p_queue  = &point_cloud_queue_pool[i];
    if (QueueUsedSize(p_queue) > POINTS_PER_FRAME) {
      //printf("%d %d %d %d\r\n", i, p_queue->rd_idx, p_queue->wr_idx, QueueUsedSize(p_queue));
      PublishPointcloudData(p_queue, POINTS_PER_FRAME);
    }
  }
}


/* control hub ---------------------------------------------------------------------------------- */

void OnSampleCallback(uint8_t status, uint8_t hub_handle, uint8_t response, void *data) {
  printf("OnSampleCallback statue %d handle %d response %d \n", status, hub_handle, response);
  if (status == kStatusSuccess) {
    if (response != 0) {
      hub.device_state = kDeviceStateConnect;
    }
  } else if (status == kStatusTimeout) {
    hub.device_state = kDeviceStateConnect;
  }
}

void OnStopSampleCallback(uint8_t status, uint8_t hub_handle, uint8_t response, void *data) {

}

void OnDeviceInformation(uint8_t status, uint8_t hub_handle, DeviceInformationResponse *ack,\
                         void *data) {
  if (status != kStatusSuccess) {
    printf("Device Query Informations Failed %d\n", status);
  }
  if (ack) {
    printf("firm ver: %d.%d.%d.%d\n",
           ack->firmware_version[0],
           ack->firmware_version[1],
           ack->firmware_version[2],
           ack->firmware_version[3]);
  }
}

void OnHubLidarInfo(uint8_t status, uint8_t handle, HubQueryLidarInformationResponse *response, void *client_data) {
  if (status != kStatusSuccess) {
    printf("Device Query Informations Failed %d\n", status);
  }
  if (response) {
    int i = 0;
    for (i = 0; i < response->count; ++i) {
      printf("Hub Lidar Info broadcast code %s id %d slot %d \n ",
             response->device_info_list[i].broadcast_code,
             response->device_info_list[i].id,
             response->device_info_list[i].slot);
    }
  }
}

void OnDeviceChange(const DeviceInfo *info, DeviceEvent type) {
  if (info == NULL) {
    return;
  }
  printf("OnDeviceChange broadcast code %s update type %d\n", info->broadcast_code, type);
  uint8_t hub_handle = info->handle;
  if (hub_handle >= kMaxLidarCount) {
    return;
  }
  if (type == kEventConnect) {
//    QueryDeviceInformation(handle, OnDeviceInformation, NULL);

    DeviceInfo *_lidars = (DeviceInfo *) malloc(sizeof(DeviceInfo) * kMaxLidarCount);

    uint8_t count = kMaxLidarCount;
    uint8_t status = GetConnectedDevices(_lidars, &count);
    if (status == kStatusSuccess) {
      int i = 0;
      for (i = 0; i < count; ++i) {
        uint8_t handle = _lidars[i].handle;
        if (handle < kMaxLidarCount) {
          lidars[handle].handle = handle;
          lidars[handle].info = _lidars[i];
          lidars[handle].device_state = kDeviceStateConnect;
          printf("lidar %d : %s\r\n", _lidars[i].handle, _lidars[i].broadcast_code);
        }
      }
    }
    if (_lidars) {
      free(_lidars);
    }
    if (info->type == kDeviceTypeHub) {
      HubQueryLidarInformation(OnHubLidarInfo, NULL);
    }
    if (hub.device_state == kDeviceStateDisconnect) {
      hub.device_state = kDeviceStateConnect;
      hub.info = *info;
    }
  } else if (type == kEventDisconnect) {
    hub.device_state = kDeviceStateDisconnect;
  } else if (type == kEventStateChange) {
    hub.info = *info;
  }

  if (hub.device_state == kDeviceStateConnect) {
    printf("Device State error_code %d\n", hub.info.status.status_code);
    printf("Device State working state %d\n", hub.info.state);
    printf("Device feature %d\n", hub.info.feature);
    if (hub.info.state == kLidarStateNormal && hub.info.status.status_code == 0) {
      HubStartSampling(OnSampleCallback, NULL);
      hub.device_state = kDeviceStateSampling;
    }
  }
}

void OnDeviceBroadcast(const BroadcastDeviceInfo *info) {
  if (info == NULL) {
    return;
  }

  //printf("Receive Broadcast Code %s\n", info->broadcast_code);
  bool found = false;

  int i = 0;
  for (i = 0; i < BROADCAST_CODE_LIST_SIZE; ++i) {
    if (strncmp(info->broadcast_code, broadcast_code_list[i], kBroadcastCodeSize) == 0) {
      found = true;
      break;
    }
  }
  if (!found) {
    return;
  }

  bool result = false;
  uint8_t hub_handle = 0;
  result = AddHubToConnect(info->broadcast_code, &hub_handle);
  if (result == kStatusSuccess && hub_handle < kMaxLidarCount) {
    SetDataCallback(hub_handle, GetLidarData);
    hub.handle = hub_handle;
    hub.device_state = kDeviceStateDisconnect;
  }
}

int main(int argc, char **argv) {

  printf("livoxtech sdk ros demo\r\n");

  PointCloudPoolInit();

  if (!Init()) {
    printf("livoxtech sdk init fail!\r\n");
    return -1;
  }

  memset(lidars, 0, sizeof(lidars));
  memset(&hub, 0, sizeof(hub));
  SetBroadcastCallback(OnDeviceBroadcast);
  SetDeviceStateUpdateCallback(OnDeviceChange);

  if (!Start()) {
    Uninit();
    return -1;
  }

  /* ros related */
  ros::init(argc, argv, "point_cloud_publisher");
  ros::NodeHandle point_cloud_node;
  cloud_pub = point_cloud_node.advertise<sensor_msgs::PointCloud>("cloud", POINTS_PER_FRAME);

  ros::Time::init();
  ros::Rate r(500); // 500 hz
  while (ros::ok()) {
    PollPointcloudData();
    r.sleep();
  }

  Uninit();
}


