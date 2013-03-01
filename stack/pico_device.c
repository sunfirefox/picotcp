/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.

.

Authors: Daniele Lacamera
*********************************************************************/


#include "pico_config.h"
#include "pico_device.h"
#include "pico_stack.h"
#include "pico_protocol.h"


RB_HEAD(pico_device_tree, pico_device);
RB_PROTOTYPE_STATIC(pico_device_tree, pico_device, node, pico_dev_cmp);

static struct pico_device_tree Device_tree;

static int pico_dev_cmp(struct pico_device *a, struct pico_device *b)
{
  if (a->hash < b->hash)
    return -1;
  if (a->hash > b->hash)
    return 1;
  return 0;
}

RB_GENERATE_STATIC(pico_device_tree, pico_device, node, pico_dev_cmp);

int pico_device_init(struct pico_device *dev, char *name, uint8_t *mac)
{
  memcpy(dev->name, name, MAX_DEVICE_NAME);
  dev->hash = pico_hash(dev->name);

  RB_INSERT(pico_device_tree, &Device_tree, dev);

  dev->q_in = pico_zalloc(sizeof(struct pico_queue));
  dev->q_out = pico_zalloc(sizeof(struct pico_queue));

  if (mac) {
    dev->eth = pico_zalloc(sizeof(struct pico_ethdev));
    memcpy(dev->eth->mac.addr, mac, PICO_SIZE_ETH);
  } else {
    dev->eth = NULL;
  }

  if (!dev->q_in || !dev->q_out || (mac && !dev->eth))
    return -1;
  return 0;
}

void pico_device_destroy(struct pico_device *dev)
{
  if (dev->destroy)
    dev->destroy(dev);

  if (dev->q_in) {
    pico_queue_empty(dev->q_in);
    pico_free(dev->q_in);
  }
  if (dev->q_out) {
    pico_queue_empty(dev->q_out);
    pico_free(dev->q_out);
  }

  if (dev->eth)
    pico_free(dev->eth);

  RB_REMOVE(pico_device_tree, &Device_tree, dev);
  pico_free(dev);
}

static int devloop(struct pico_device *dev, int loop_score, int direction)
{
  struct pico_frame *f;

  /* If device supports polling, give control. Loop score is managed internally, 
   * remaining loop points are returned. */
  if (dev->poll) {
    loop_score = dev->poll(dev, loop_score);
  }

  if (direction == PICO_LOOP_DIR_OUT) {

    while(loop_score > 0) {
      if (dev->q_out->frames <= 0)
        break;

      /* Device dequeue + send */
      f = pico_dequeue(dev->q_out);
      if (f) {
        if (dev->eth) {
          int ret = pico_ethernet_send(f);
          if (0 == ret) {
            loop_score--;
            continue;
          } if (ret < 0) {
            if (!pico_source_is_local(f)) { 
              dbg("Destination unreachable -------> SEND ICMP\n");
              pico_notify_dest_unreachable(f);
            } else {
              dbg("Destination unreachable -------> LOCAL\n");
            }
            pico_frame_discard(f);
            continue;
          }
        } else {
          dev->send(dev, f->start, f->len);
        }
        pico_frame_discard(f);
        loop_score--;
      }
    }

  } else if (direction == PICO_LOOP_DIR_IN) {

    while(loop_score > 0) {
      if (dev->q_in->frames <= 0)
        break;

      /* Receive */
      f = pico_dequeue(dev->q_in);
      if (f) {
        if (dev->eth) {
          f->datalink_hdr = f->buffer;
          pico_ethernet_receive(f);
        } else {
          f->net_hdr = f->buffer;
          pico_network_receive(f);
        }
        loop_score--;
      }
    }
  }

  return loop_score;
}


#define DEV_LOOP_MIN  16

int pico_devices_loop(int loop_score, int direction)
{
  struct pico_device *start;
  static struct pico_device *next = NULL, *next_in = NULL, *next_out = NULL;

  if (next_in == NULL) {
    next_in = RB_MIN(pico_device_tree, &Device_tree);
  }
  if (next_out == NULL) {
    next_out = RB_MIN(pico_device_tree, &Device_tree);
  }
  
  if (direction == PICO_LOOP_DIR_IN)
    next = next_in;
  else if (direction == PICO_LOOP_DIR_OUT)
    next = next_out;

  /* init start node */
  start = next;

  /* round-robin all devices, break if traversed all devices */
  while (loop_score > DEV_LOOP_MIN && next != NULL) {
    loop_score = devloop(next, loop_score, direction);

    next = RB_NEXT(pico_device_tree, &Device_tree, next);
    if (next == NULL)
      next = RB_MIN(pico_device_tree, &Device_tree);
    if (next == start)
      break;
  }

  if (direction == PICO_LOOP_DIR_IN)
    next_in = next;
  else if (direction == PICO_LOOP_DIR_OUT)
    next_out = next;

  return loop_score;
}

struct pico_device* pico_get_device(char* name)
{
  struct pico_device *dev;
  RB_FOREACH(dev, pico_device_tree, &Device_tree) {
    if(strcmp(name, dev->name) == 0)
      return dev;
  }
  return NULL;
}
