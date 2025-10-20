/*
* Copyright (c) 2020 - 2025 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "rm_netxduo_ether.h"

#include "nx_arp.h"
#include "nx_rarp.h"

#if BSP_PERIPHERAL_ESWM_PRESENT
 #include "r_rmac.h"
#else
 #include "r_ether.h"
#endif

/***********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/

/* Disable support for RARP by default. */
#ifndef RM_NETXDUO_ETHER_RARP_SUPPORT
 #define RM_NETXDUO_ETHER_RARP_SUPPORT    (0U)
#endif

#define NX_ETHERNET_SIZE                  14

/* Ethernet Frame IDs. */
#define NX_ETHERNET_IP                    0x0800
#define NX_ETHERNET_ARP                   0x0806
#define NX_ETHERNET_RARP                  0x8035
#define NX_ETHERNET_IPV6                  0x86DD

#define NX_ETHERNET_MIN_TRANSMIT_SIZE     60U

#ifndef NX_ETHERNET_POLLING_INTERVAL
 #define NX_ETHERNET_POLLING_INTERVAL     (10U)
#endif

#ifndef NX_ETHERNET_POLLING_TIMEOUT
 #define NX_ETHERNET_POLLING_TIMEOUT      (100U)
#endif

/* Transmit Complete. */
#define ETHER_ISR_EE_TC_MASK              (1U << 21U)

/* Frame Receive. */
#define ETHER_ISR_EE_FR_MASK              (1U << 18U)

/* Instance control is used to check if instance is opened */
#if BSP_PERIPHERAL_ESWM_PRESENT
 #define ETHER_INSTANCE_CTRL_T            rmac_instance_ctrl_t
#else
 #define ETHER_INSTANCE_CTRL_T            ether_instance_ctrl_t
#endif

/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/

static void rm_netxduo_ether_cleanup(rm_netxduo_ether_instance_t * p_netxduo_ether_instance);
void        rm_netxduo_ether_receive_packet(rm_netxduo_ether_instance_t * p_netxduo_ether_instance);
void        rm_netxduo_ether_receive_process_packet(rm_netxduo_ether_instance_t * p_netxduo_ether_instance,
                                                    NX_PACKET                   * p_nx_packet,
                                                    uint32_t                      length);

#if !RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT
static void rm_netxduo_receive_thread_entry(ULONG thread_input);

#endif
void rm_event_timer_callback(ULONG data);

/*******************************************************************************************************************//**
 * Ethernet driver for NetX Duo.
 **********************************************************************************************************************/
void rm_netxduo_ether (NX_IP_DRIVER * driver_req_ptr, rm_netxduo_ether_instance_t * p_netxduo_ether_instance)
{
    ether_instance_t const * p_ether_instance = p_netxduo_ether_instance->p_cfg->p_ether_instance;
    NX_INTERFACE           * interface_ptr    = driver_req_ptr->nx_ip_driver_interface;

    p_netxduo_ether_instance->p_ctrl->p_interface = interface_ptr;

    p_netxduo_ether_instance->p_ctrl->p_ip = driver_req_ptr->nx_ip_driver_ptr;

    driver_req_ptr->nx_ip_driver_status = NX_SUCCESS;

    switch (driver_req_ptr->nx_ip_driver_command)
    {
        case NX_LINK_INITIALIZE:
        {
            /* The nx_interface_ip_mtu_size should be the MTU for the IP payload.
             * For regular Ethernet, the IP MTU is 1500. */
            nx_ip_interface_mtu_set(driver_req_ptr->nx_ip_driver_ptr,
                                    interface_ptr->nx_interface_index,
                                    p_netxduo_ether_instance->p_cfg->mtu);

            /* Set the physical address (MAC address) of this IP instance.  */
            uint8_t * p_mac_address = p_ether_instance->p_cfg->p_mac_address;
            nx_ip_interface_physical_address_set(driver_req_ptr->nx_ip_driver_ptr,
                                                 interface_ptr->nx_interface_index,
                                                 (ULONG) ((p_mac_address[0] << 8) | (p_mac_address[1] << 0)),
                                                 (ULONG) ((p_mac_address[2] << 24) | (p_mac_address[3] << 16) |
                                                          (p_mac_address[4] << 8) |
                                                          (p_mac_address[5] << 0)),
                                                 NX_FALSE);

            /* Indicate to the IP software that IP to physical mapping is required.  */
            nx_ip_interface_address_mapping_configure(driver_req_ptr->nx_ip_driver_ptr,
                                                      interface_ptr->nx_interface_index,
                                                      NX_TRUE);
            break;
        }

        case NX_LINK_ENABLE:
        {
#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

            /* Initialize ether driver state. */
            p_netxduo_ether_instance->p_ctrl->tx_packet_index             = 0;
            p_netxduo_ether_instance->p_ctrl->tx_packet_transmitted_index = 0;
            p_netxduo_ether_instance->p_ctrl->rx_packet_index             = 0;
#endif

            /* Clear the multicast MAC address list. */
            memset(p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses, 0,
                   sizeof(p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses));

#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT
            for (int i = 0; i < p_ether_instance->p_cfg->num_tx_descriptors; i++)
            {
                p_netxduo_ether_instance->p_cfg->p_tx_packets[i] = NULL;
            }
#endif

            /* Set the return status to be invalid if the link is not enabled. */
            driver_req_ptr->nx_ip_driver_status = NX_NOT_ENABLED;
            p_netxduo_ether_instance->p_ctrl->p_interface->nx_interface_link_up = NX_FALSE;

            /* Create ether tx semaphore in order to synchronize ether task with transmit complete interrupts. */
            if (TX_SUCCESS !=
                tx_semaphore_create(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore, "ether_tx_semaphore",
                                    p_ether_instance->p_cfg->num_tx_descriptors))
            {
                FSP_LOG_PRINT("Failed to create ether_tx_semaphore.");
            }

#if !RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

            /* Create ether rx semaphore in order to synchronize receive thread with receive complete interrupts. */
            if (TX_SUCCESS !=
                tx_semaphore_create(&p_netxduo_ether_instance->p_ctrl->ether_rx_semaphore, "ether_rx_semaphore", 0))
            {
                FSP_LOG_PRINT("Failed to create ether_rx_semaphore.");
            }

            UINT priority = 0;

            /* Get priority of IP thread.  */
            tx_thread_info_get(tx_thread_identify(),
                               NX_NULL,
                               NX_NULL,
                               NX_NULL,
                               &priority,
                               NX_NULL,
                               NX_NULL,
                               NX_NULL,
                               NX_NULL);

            if (0 != priority)
            {
                priority -= 1;
            }

            /* Create the receive thread.  */
            /* The priority of receive thread is higher than IP thread.  */
            if (TX_SUCCESS !=
                tx_thread_create(&p_netxduo_ether_instance->p_ctrl->receive_thread, "Receive Thread",
                                 rm_netxduo_receive_thread_entry, (ULONG) p_netxduo_ether_instance,
                                 p_netxduo_ether_instance->p_ctrl->receive_thread_stack,
                                 RM_NETXDUO_ETHER_RECEIVE_THREAD_STACK_SIZE,
                                 priority, priority, TX_NO_TIME_SLICE, TX_AUTO_START))
            {
                FSP_LOG_PRINT("Failed to create receive_thread.");
            }
#endif

            /* Open the r_ether driver. */
            else if (FSP_SUCCESS != p_ether_instance->p_api->open(p_ether_instance->p_ctrl, p_ether_instance->p_cfg))
            {
                FSP_LOG_PRINT("Failed to open Ethernet driver.");
                tx_semaphore_delete(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore);
            }
            /* Create a timer to poll for completion of auto negotiation. */
            else if (TX_SUCCESS !=
                     tx_timer_create(&p_netxduo_ether_instance->p_ctrl->event_timer, (CHAR *) "nx ether driver timer",
                                     rm_event_timer_callback, (ULONG) p_netxduo_ether_instance,
                                     NX_ETHERNET_POLLING_INTERVAL,
                                     NX_ETHERNET_POLLING_INTERVAL, TX_AUTO_ACTIVATE))
            {
                FSP_LOG_PRINT("Failed to create \"nx ether driver timer\".");
                tx_semaphore_delete(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore);
                p_ether_instance->p_api->close(p_ether_instance->p_ctrl);
            }
            else
            {
                driver_req_ptr->nx_ip_driver_status = NX_SUCCESS;
            }

            break;
        }

        case NX_LINK_DISABLE:
        {
            /* Delete RTOS objects. */
            rm_netxduo_ether_cleanup(p_netxduo_ether_instance);

            break;
        }

        case NX_LINK_PACKET_SEND:
        case NX_LINK_PACKET_BROADCAST:
        case NX_LINK_ARP_SEND:
        case NX_LINK_ARP_RESPONSE_SEND:
        case NX_LINK_RARP_SEND:
        {
            if (NX_TRUE != interface_ptr->nx_interface_link_up)
            {
                /* THe link has not been established yet. */
                driver_req_ptr->nx_ip_driver_status = NX_INVALID_INTERFACE;

                FSP_LOG_PRINT("Cannot transmit Ethernet packets because the link is down.");

                /* Release the NetX packet because it cannot be sent. */
                if (NX_SUCCESS != nx_packet_transmit_release(driver_req_ptr->nx_ip_driver_packet))
                {
                    FSP_LOG_PRINT("Failed to release transmit packet.");
                }

                return;
            }

            NX_PACKET * packet_ptr = driver_req_ptr->nx_ip_driver_packet;

            /* Adjust prepend pointer to make room for Ethernet header. */
            UCHAR * p_packet_prepend = packet_ptr->nx_packet_prepend_ptr - NX_ETHERNET_SIZE;
            UINT    packet_length    = packet_ptr->nx_packet_length + NX_ETHERNET_SIZE;

            /* Setup the Ethernet frame pointer to build the Ethernet frame.  Backup another 2
             * bytes to get 32-bit word alignment.  */
            ULONG * ethernet_frame_ptr = (ULONG *) (p_packet_prepend - 2);

            /* Build the Ethernet frame.  */
            *ethernet_frame_ptr       = driver_req_ptr->nx_ip_driver_physical_address_msw;
            *(ethernet_frame_ptr + 1) = driver_req_ptr->nx_ip_driver_physical_address_lsw;
            *(ethernet_frame_ptr + 2) = (interface_ptr->nx_interface_physical_address_msw << 16) |
                                        (interface_ptr->nx_interface_physical_address_lsw >> 16);
            *(ethernet_frame_ptr + 3) = (interface_ptr->nx_interface_physical_address_lsw << 16);

            if (driver_req_ptr->nx_ip_driver_command == NX_LINK_ARP_SEND)
            {
                *(ethernet_frame_ptr + 3) |= NX_ETHERNET_ARP;
            }
            else if (driver_req_ptr->nx_ip_driver_command == NX_LINK_ARP_RESPONSE_SEND)
            {
                *(ethernet_frame_ptr + 3) |= NX_ETHERNET_ARP;
            }

#if RM_NETXDUO_ETHER_RARP_SUPPORT
            else if (driver_req_ptr->nx_ip_driver_command == NX_LINK_RARP_SEND)
            {
                *(ethernet_frame_ptr + 3) |= NX_ETHERNET_RARP;
            }
#endif
            else if (packet_ptr->nx_packet_ip_version == 4)
            {
                *(ethernet_frame_ptr + 3) |= NX_ETHERNET_IP;
            }
            else
            {
                *(ethernet_frame_ptr + 3) |= NX_ETHERNET_IPV6;
            }

            /* Endian swapping if NX_LITTLE_ENDIAN is defined.  */
            NX_CHANGE_ULONG_ENDIAN(*(ethernet_frame_ptr));
            NX_CHANGE_ULONG_ENDIAN(*(ethernet_frame_ptr + 1));
            NX_CHANGE_ULONG_ENDIAN(*(ethernet_frame_ptr + 2));
            NX_CHANGE_ULONG_ENDIAN(*(ethernet_frame_ptr + 3));

            /* Zero-pad if the packet is smaller than the minimum packet size. */
            if (packet_length < NX_ETHERNET_MIN_TRANSMIT_SIZE)
            {
                memset(p_packet_prepend + packet_length, 0, NX_ETHERNET_MIN_TRANSMIT_SIZE - packet_length);
                packet_length = NX_ETHERNET_MIN_TRANSMIT_SIZE;
            }

            if (TX_SUCCESS !=
                tx_semaphore_get(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore, NX_IP_PERIODIC_RATE))
            {
                /* Set driver status to indicate that the packet was not transmitted. */
                driver_req_ptr->nx_ip_driver_status = NX_TX_QUEUE_DEPTH;

                FSP_LOG_PRINT("No space in the Ethernet transmit queue.");

                /* Release the NetX packet because it cannot be sent. */
                if (NX_SUCCESS != nx_packet_transmit_release(driver_req_ptr->nx_ip_driver_packet))
                {
                    FSP_LOG_PRINT("Failed to release transmit packet.");
                }

                return;
            }

#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

            /* Store the NetX Packet so that it can be released after transmission is completed. */
            UINT index = p_netxduo_ether_instance->p_ctrl->tx_packet_index;
            p_netxduo_ether_instance->p_cfg->p_tx_packets[index] = packet_ptr;
#endif

            /* Transmit the Ethernet packet. */
            fsp_err_t err = p_ether_instance->p_api->write(p_ether_instance->p_ctrl, p_packet_prepend, packet_length);

#if !RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

            /* Release the packet since the data was either copied into ether buffers or the send failed */
            if (NX_SUCCESS != nx_packet_transmit_release(driver_req_ptr->nx_ip_driver_packet))
            {
                FSP_LOG_PRINT("Failed to release transmit packet.");
            }
#endif

            if (FSP_SUCCESS != err)
            {
                /* Set driver status to indicate that the packet was not transmitted. */
                driver_req_ptr->nx_ip_driver_status = NX_TX_QUEUE_DEPTH;

                FSP_LOG_PRINT("Failed to transmit the Ethernet packet.");

#if !RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

                /* Return semaphore since interrupt won't happen */
                if (TX_SUCCESS !=
                    tx_semaphore_ceiling_put(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore,
                                             p_ether_instance->p_cfg->num_tx_descriptors))
                {
                    FSP_LOG_PRINT("Failed to increment tx semaphore.");
                }
#endif

                return;
            }

#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

            /* Increment the index used to store the NetX packet. */
            p_netxduo_ether_instance->p_ctrl->tx_packet_index = (index + 1U) %
                                                                p_ether_instance->p_cfg->num_tx_descriptors;
#endif

            break;
        }

        case NX_LINK_MULTICAST_JOIN:
        {
            /* Store the multicast MAC address in list at the next available location. */
            uint32_t i;
            for (i = 0; i < NX_MAX_MULTICAST_GROUPS; i++)
            {
                if ((p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses[i].mac_address_msw == 0U) &&
                    (p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses[i].mac_address_lsw == 0U))
                {
                    p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses[i].mac_address_msw =
                        driver_req_ptr->nx_ip_driver_physical_address_msw;
                    p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses[i].mac_address_lsw =
                        driver_req_ptr->nx_ip_driver_physical_address_lsw;
                    break;
                }
            }

            if (NX_MAX_MULTICAST_GROUPS == i)
            {
                driver_req_ptr->nx_ip_driver_status = NX_NO_MORE_ENTRIES;
            }

            break;
        }

        case NX_LINK_MULTICAST_LEAVE:
        {
            /* Remove the multicast MAC address from the list. */
            uint32_t i;
            for (i = 0; i < NX_MAX_MULTICAST_GROUPS; i++)
            {
                if ((p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses[i].mac_address_msw ==
                     driver_req_ptr->nx_ip_driver_physical_address_msw) &&
                    (p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses[i].mac_address_lsw ==
                     driver_req_ptr->nx_ip_driver_physical_address_lsw))
                {
                    p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses[i].mac_address_msw = 0U;
                    p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses[i].mac_address_lsw = 0U;
                    break;
                }
            }

            if (NX_MAX_MULTICAST_GROUPS == i)
            {
                driver_req_ptr->nx_ip_driver_status = NX_ENTRY_NOT_FOUND;
            }

            break;
        }

        case NX_LINK_DEFERRED_PROCESSING:
        {
            /* linkProcess will call the Ethernet callback from the ip task when the link changes. */
            if (0U != ((ETHER_INSTANCE_CTRL_T *) p_ether_instance->p_ctrl)->open)
            {
                p_ether_instance->p_api->linkProcess(p_ether_instance->p_ctrl);
            }

            break;
        }

        case NX_LINK_GET_STATUS:
        {
            /* Return the link status in the supplied return pointer.  */
            *(driver_req_ptr->nx_ip_driver_return_ptr) = interface_ptr->nx_interface_link_up;
            break;
        }

        case NX_LINK_SET_PHYSICAL_ADDRESS:
        {
            if (p_netxduo_ether_instance->p_ctrl->p_interface->nx_interface_link_up)
            {
                /* The MAC address cannot be changed while the link is up. */
                FSP_LOG_PRINT("Failed to update the MAC address because the link is up.");
                driver_req_ptr->nx_ip_driver_status = NX_NOT_SUPPORTED;
            }
            else
            {
                uint8_t * p_mac_address = p_ether_instance->p_cfg->p_mac_address;

                uint32_t physical_msw = driver_req_ptr->nx_ip_driver_physical_address_msw;
                uint32_t physical_lsw = driver_req_ptr->nx_ip_driver_physical_address_lsw;

                /* Update the MAC address in the r_ether configuration. */
                p_mac_address[0] = (uint8_t) (physical_msw >> 8U) & UINT8_MAX;
                p_mac_address[1] = (uint8_t) (physical_msw >> 0U) & UINT8_MAX;
                p_mac_address[2] = (uint8_t) (physical_lsw >> 24U) & UINT8_MAX;
                p_mac_address[3] = (uint8_t) (physical_lsw >> 16U) & UINT8_MAX;
                p_mac_address[4] = (uint8_t) (physical_lsw >> 8U) & UINT8_MAX;
                p_mac_address[5] = (uint8_t) (physical_lsw >> 0U) & UINT8_MAX;
            }

            break;
        }

        default:
        {
            break;
        }
    }
}

/*******************************************************************************************************************//**
 * Delete all unused rtos objects.
 **********************************************************************************************************************/
static void rm_netxduo_ether_cleanup (rm_netxduo_ether_instance_t * p_netxduo_ether_instance)
{
    /* Set interface link status to down. */
    p_netxduo_ether_instance->p_ctrl->p_interface->nx_interface_link_up = NX_FALSE;

    ether_instance_t const * p_ether_instance = p_netxduo_ether_instance->p_cfg->p_ether_instance;

    CHAR         * semaphore_name;
    ULONG          semaphore_current_value;
    TX_THREAD    * semaphore_first_suspended;
    ULONG          semaphore_suspended_count;
    TX_SEMAPHORE * semaphore_next_semaphore;
    uint32_t       timeout_count = NX_ETHERNET_POLLING_TIMEOUT;

    /* Wait for all transmit packets to be sent before closing the Ethernet driver.
     * If the ETHERC peripheral is reset while packets are being transmitted, then
     * invalid data may be observed on the TXD lines.
     *
     * See "EDMAC Mode Register (EDMR)" description in the relevant hardware manual.
     */
    while (1)
    {
        _txe_semaphore_info_get(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore,
                                &semaphore_name,
                                &semaphore_current_value,
                                &semaphore_first_suspended,
                                &semaphore_suspended_count,
                                &semaphore_next_semaphore);

        if ((semaphore_current_value < p_ether_instance->p_cfg->num_tx_descriptors) && --timeout_count)
        {
            tx_thread_sleep(NX_ETHERNET_POLLING_INTERVAL);
        }
        else
        {
            break;
        }
    }

    if (0 == timeout_count)
    {
        FSP_LOG_PRINT("Failed to transmit remaining packets.");
    }

    /* Close the ether driver. */
    p_ether_instance->p_api->close(p_ether_instance->p_ctrl);

#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

    /* Reset the tx packet index. */
    p_netxduo_ether_instance->p_ctrl->tx_packet_index             = 0;
    p_netxduo_ether_instance->p_ctrl->tx_packet_transmitted_index = 0;
#endif

    /* Delete RTOS Objects. */
    tx_semaphore_delete(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore);
    tx_timer_deactivate(&p_netxduo_ether_instance->p_ctrl->event_timer);
    tx_timer_delete(&p_netxduo_ether_instance->p_ctrl->event_timer);
#if !RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT
    tx_thread_terminate(&p_netxduo_ether_instance->p_ctrl->receive_thread);
    tx_thread_delete(&p_netxduo_ether_instance->p_ctrl->receive_thread);
    tx_semaphore_delete(&p_netxduo_ether_instance->p_ctrl->ether_rx_semaphore);
#else

    /* Release all transmit packets. */
    for (uint32_t i = 0; i < p_ether_instance->p_cfg->num_tx_descriptors; i++)
    {
        if (NULL != p_netxduo_ether_instance->p_cfg->p_tx_packets[i])
        {
            nx_packet_release(p_netxduo_ether_instance->p_cfg->p_tx_packets[i]);
            p_netxduo_ether_instance->p_cfg->p_tx_packets[i] = NULL;
        }
    }

    /* Release all receive packets. */
    for (uint32_t i = 0; i < p_ether_instance->p_cfg->num_rx_descriptors; i++)
    {
        nx_packet_release(p_netxduo_ether_instance->p_cfg->p_rx_packets[i]);
    }
#endif
}

#if !RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT
static void rm_netxduo_receive_thread_entry (ULONG thread_input)
{
    rm_netxduo_ether_instance_t * p_netxduo_ether_instance = (rm_netxduo_ether_instance_t *) thread_input;

    while (1)
    {
        if (TX_SUCCESS == tx_semaphore_get(&p_netxduo_ether_instance->p_ctrl->ether_rx_semaphore, TX_WAIT_FOREVER))
        {
            rm_netxduo_ether_receive_packet(p_netxduo_ether_instance);
        }
    }
}

#endif

/*******************************************************************************************************************//**
 * Process all Ethernet packets that have been received.
 **********************************************************************************************************************/
void rm_netxduo_ether_receive_packet (rm_netxduo_ether_instance_t * p_netxduo_ether_instance)
{
#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT
    NX_PACKET ** p_nx_buffers = p_netxduo_ether_instance->p_cfg->p_rx_packets;
#endif
    ether_instance_t const * p_ether_instance = p_netxduo_ether_instance->p_cfg->p_ether_instance;
    fsp_err_t                err              = FSP_SUCCESS;
    NX_PACKET              * p_nx_packet      = NULL;
    do
    {
#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

        /* Get a pointer to the packet received. */
        uint8_t * p_buffer_out;
        uint32_t  length;
        err = p_ether_instance->p_api->read(p_ether_instance->p_ctrl, &p_buffer_out, &length);
        if (FSP_SUCCESS != err)
        {
            break;
        }
#endif

        /* Allocate NetX packet to copy data into. */
        if (NX_SUCCESS !=
            nx_packet_allocate(p_netxduo_ether_instance->p_ctrl->p_ip->nx_ip_default_packet_pool, &p_nx_packet,
                               NX_RECEIVE_PACKET, NX_NO_WAIT))
        {
            /* If a NetX packet could not be allocated, then the received packet must be dropped in order to receive the next packet. */
            FSP_LOG_PRINT("Failed to allocate NetX Packet.");

#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

            /* Update the buffer pointer in the buffer descriptor. */
            if (FSP_SUCCESS != p_ether_instance->p_api->rxBufferUpdate(p_ether_instance->p_ctrl, p_buffer_out))
            {
                FSP_LOG_PRINT("Failed to update buffer in r_ether driver.");
            }

#else
            err = FSP_ERR_ASSERTION;
#endif
        }
        else
        {
#if BSP_PERIPHERAL_ESWM_PRESENT

            /*
             * RMAC doesn't have specific alignment requirements but doesn't support inserting padding so instead
             * align the ether packet to 4 bytes + 2 so that the IP header will always be 4-byte aligned.
             */
            p_nx_packet->nx_packet_prepend_ptr =
                (UCHAR *) ((((uint32_t) p_nx_packet->nx_packet_prepend_ptr + 3U) & ~(3U)) + 2);
#else

            /*
             * Make sure that the buffer is 32 byte aligned (See "Receive descriptor" in the EDMAC section
             * of the relevant hardware manual)
             */
            p_nx_packet->nx_packet_prepend_ptr =
                (UCHAR *) (((uint32_t) p_nx_packet->nx_packet_prepend_ptr + 31U) & ~(31U));
#endif

            p_nx_packet->nx_packet_address.nx_packet_interface_ptr = p_netxduo_ether_instance->p_ctrl->p_interface;

#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

            /* Update the buffer pointer in the buffer descriptor. */
            if (FSP_SUCCESS !=
                p_ether_instance->p_api->rxBufferUpdate(p_ether_instance->p_ctrl, p_nx_packet->nx_packet_prepend_ptr))
            {
                FSP_LOG_PRINT("Failed to update buffer in r_ether driver.");
                break;
            }

            /* Get the index of the NetX packet that was received. */
            uint32_t index = p_netxduo_ether_instance->p_ctrl->rx_packet_index;

            /*
             * If invalid Ethernet frames were received or if the driver previously failed to allocate a NetX packet, the index will need to be
             * incremented in order to get the NetX packet associated with the current buffer that has been received.
             */
            for (uint32_t i = 0;
                 p_nx_buffers[index]->nx_packet_prepend_ptr != p_buffer_out &&
                 i < p_ether_instance->p_cfg->num_rx_descriptors;
                 i++)
            {
                index = (index + 1) % p_ether_instance->p_cfg->num_rx_descriptors; // NOLINT(clang-analyzer-core.DivideZero)
                FSP_LOG_PRINT("Skipping to the next NetX packet.");
            }

            /* Save the expected index of the next NetX packet index. */
            p_netxduo_ether_instance->p_ctrl->rx_packet_index = (index + 1) % // NOLINT(clang-analyzer-core.DivideZero)
                                                                p_ether_instance->p_cfg->num_rx_descriptors;

            /* Verify that the NetX packet was found. */
            if (p_nx_buffers[index]->nx_packet_prepend_ptr != p_buffer_out)
            {
                FSP_LOG_PRINT("Could not find the NetX packet associated with the received buffer.");
            }
            else
            {
                rm_netxduo_ether_receive_process_packet(p_netxduo_ether_instance, p_nx_buffers[index], length);
            }

            /* Store pointer to the newly allocated NetX Packet at the index where it was written into the r_ether buffer descriptor. */
            p_nx_buffers[index] = p_nx_packet;
#else
            uint32_t length;
            err = p_ether_instance->p_api->read(p_ether_instance->p_ctrl, p_nx_packet->nx_packet_prepend_ptr, &length);
            if (FSP_SUCCESS != err)
            {
                if (NX_SUCCESS != nx_packet_release(p_nx_packet))
                {
                    FSP_LOG_PRINT("Failed to release NetX Packet.");
                }

                break;
            }
            rm_netxduo_ether_receive_process_packet(p_netxduo_ether_instance, p_nx_packet, length);
#endif
        }
    } while (FSP_SUCCESS == err);
}

/*******************************************************************************************************************//**
 * Process Ethernet packet.
 **********************************************************************************************************************/
void rm_netxduo_ether_receive_process_packet (rm_netxduo_ether_instance_t * p_netxduo_ether_instance,
                                              NX_PACKET                   * p_nx_packet,
                                              uint32_t                      length)
{
    ether_instance_t const * p_ether_instance = p_netxduo_ether_instance->p_cfg->p_ether_instance;
    uint8_t                * p_mac_address    = p_ether_instance->p_cfg->p_mac_address;
    ULONG mac_msw = (ULONG) ((p_mac_address[0] << 8) | (p_mac_address[1] << 0));
    ULONG mac_lsw =
        (ULONG) ((p_mac_address[2] << 24) | (p_mac_address[3] << 16) | (p_mac_address[4] << 8) |
                 (p_mac_address[5] << 0));

    /* Pick up the destination MAC address from the packet.  */
    ULONG destination_address_msw = (ULONG) *(p_nx_packet->nx_packet_prepend_ptr);
    destination_address_msw = (destination_address_msw << 8) |
                              (ULONG) *(p_nx_packet->nx_packet_prepend_ptr + 1);
    ULONG destination_address_lsw = (ULONG) *(p_nx_packet->nx_packet_prepend_ptr + 2);
    destination_address_lsw = (destination_address_lsw << 8) |
                              (ULONG) *(p_nx_packet->nx_packet_prepend_ptr + 3);
    destination_address_lsw = (destination_address_lsw << 8) |
                              (ULONG) *(p_nx_packet->nx_packet_prepend_ptr + 4);
    destination_address_lsw = (destination_address_lsw << 8) |
                              (ULONG) *(p_nx_packet->nx_packet_prepend_ptr + 5);

    bool multicast_group = false;

    /* Check if the packet is an IPv4 Multicast packet. */
    if ((destination_address_msw == 0x00000100U) && ((destination_address_lsw >> 24U) == 0x5EU)) // NOLINT(readability-magic-numbers)
    {
        /* Check if the IP instance is a member of the group. */
        for (uint32_t i = 0; i < NX_MAX_MULTICAST_GROUPS; i++)
        {
            /* IPv4 multicast MAC addresses always begin with 0x0100 so only destination_address_lsw needs to be checked. */
            if (destination_address_lsw ==
                p_netxduo_ether_instance->p_ctrl->multicast_mac_addresses[i].mac_address_lsw)
            {
                multicast_group = true;
                break;
            }
        }
    }

    /* Only process packets that are meant for this mac address (dest=Broadcast/mac_address). */
    if (((destination_address_msw == ((ULONG) 0x0000FFFF)) &&  // NOLINT(readability-magic-numbers)
         (destination_address_lsw == ((ULONG) 0xFFFFFFFF))) || // NOLINT(readability-magic-numbers)
        ((destination_address_msw == mac_msw) &&               // NOLINT(readability-magic-numbers)
         (destination_address_lsw == mac_lsw)) ||              // NOLINT(readability-magic-numbers)
        (destination_address_msw == ((ULONG) 0x00003333)) ||   // NOLINT(readability-magic-numbers)
        ((destination_address_msw == 0) && (destination_address_lsw == 0)) ||
        multicast_group)
    {
        /* Get the Ethernet packet id. */
        UINT packet_type = (((UINT) (*(p_nx_packet->nx_packet_prepend_ptr + 12))) << 8) |
                           ((UINT) (*(p_nx_packet->nx_packet_prepend_ptr + 13)));

        if ((packet_type == NX_ETHERNET_IP) ||
#ifndef NX_DISABLE_IPV6
            (packet_type == NX_ETHERNET_IPV6) ||
#endif
            (packet_type == NX_ETHERNET_ARP)
#if RM_NETXDUO_ETHER_RARP_SUPPORT
            || (packet_type == NX_ETHERNET_RARP)
#endif
            )
        {
            /* Update length. */
            p_nx_packet->nx_packet_length = length;

            /* Move the append ptr to the new end of data. */
            p_nx_packet->nx_packet_append_ptr = p_nx_packet->nx_packet_prepend_ptr +
                                                p_nx_packet->nx_packet_length;

            /* Remove the Ethernet packet header. */
            uint32_t padding = p_ether_instance->p_cfg->padding;
            p_nx_packet->nx_packet_prepend_ptr += (NX_ETHERNET_SIZE + padding);
            p_nx_packet->nx_packet_length      -= (NX_ETHERNET_SIZE + padding);

            switch (packet_type)
            {
                case NX_ETHERNET_IP:
#ifndef NX_DISABLE_IPV6
                case NX_ETHERNET_IPV6:
#endif
                    {
                        /* Process the IP packet. */
                        _nx_ip_packet_deferred_receive(p_netxduo_ether_instance->p_ctrl->p_ip, p_nx_packet);
                        break;
                    }

                case NX_ETHERNET_ARP:
                {
                    /* Process the ARP packet. */
                    _nx_arp_packet_deferred_receive(p_netxduo_ether_instance->p_ctrl->p_ip, p_nx_packet);
                    break;
                }

#if RM_NETXDUO_ETHER_RARP_SUPPORT
                case NX_ETHERNET_RARP:
                {
                    /* Process the RARP packet. */
                    _nx_rarp_packet_deferred_receive(p_netxduo_ether_instance->p_ctrl->p_ip, p_nx_packet);
                    break;
                }
#endif

                default:
                {
                    break;
                }
            }
        }
        else
        {
            if (NX_SUCCESS != nx_packet_release(p_nx_packet))
            {
                FSP_LOG_PRINT("Failed to release NetX Packet.");
            }
        }
    }
    else
    {
        if (NX_SUCCESS != nx_packet_release(p_nx_packet))
        {
            FSP_LOG_PRINT("Failed to release NetX Packet.");
        }
    }
}

/*******************************************************************************************************************//**
 * Timer callback for notifying the driver to poll the Ethernet link status.
 **********************************************************************************************************************/
void rm_event_timer_callback (ULONG data)
{
    rm_netxduo_ether_instance_t * p_netxduo_ether_instance = (rm_netxduo_ether_instance_t *) data;

    /* Call NetX to register a deferred event.  */
    _nx_ip_driver_deferred_processing(p_netxduo_ether_instance->p_ctrl->p_ip);
}

/*******************************************************************************************************************//**
 * Callback from r_ether ISR.
 *
 * Notifies ether_thread when a packet is received using a semaphore.
 * Processes Link Up and Link Down events.
 **********************************************************************************************************************/
void rm_netxduo_ether_callback (ether_callback_args_t * p_args)
{
    /* Get the ether interface from a global pointer because r_ether does not set p_contect in p_args. */
    rm_netxduo_ether_instance_t * p_netxduo_ether_instance = (rm_netxduo_ether_instance_t *) p_args->p_context;
    ether_instance_t const      * p_ether_instance         = p_netxduo_ether_instance->p_cfg->p_ether_instance;

    /* Either the callback was called from an ISR or it was called from the linkProcess. */
    switch (p_args->event)
    {
        /* Packet Transmitted. */
        case ETHER_EVENT_TX_COMPLETE:
        {
#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT
            uint32_t tx_packet_transmitted_index = p_netxduo_ether_instance->p_ctrl->tx_packet_transmitted_index;
            if (NULL != p_netxduo_ether_instance->p_cfg->p_tx_packets[tx_packet_transmitted_index])
            {
                uint8_t * p_buffer_current = NULL;
                uint8_t * p_buffer_last    = NULL;

                /* Get the pointer to the last buffer that was transmitted. */
                if (FSP_SUCCESS != p_ether_instance->p_api->txStatusGet(p_ether_instance->p_ctrl, &p_buffer_last))
                {
                    FSP_LOG_PRINT("Failed to get the last buffer transmitted.");
                }
                else
                {
                    do
                    {
                        /* Get the pointer to the next NX_PACKET to release. */
                        NX_PACKET * p_nx_packet_current =
                            p_netxduo_ether_instance->p_cfg->p_tx_packets[tx_packet_transmitted_index];

                        if (NULL != p_nx_packet_current)
                        {
                            /* Calculate the pointer to the start of the Ethernet packet that was transmitted. */
                            p_buffer_current = p_nx_packet_current->nx_packet_prepend_ptr - NX_ETHERNET_SIZE;

                            /* Release the NX_PACKET. */
                            if (TX_SUCCESS != nx_packet_transmit_release(p_nx_packet_current))
                            {
                                FSP_LOG_PRINT("Failed to release NetX transmit Packet.");
                            }

                            /* Remove the NX_PACKET from the transmit queue. */
                            p_netxduo_ether_instance->p_cfg->p_tx_packets[tx_packet_transmitted_index] = NULL;

                            /* Synchronize the IP task with the transmit complete ISR. */
                            if (TX_SUCCESS !=
                                tx_semaphore_ceiling_put(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore,
                                                         p_ether_instance->p_cfg->num_tx_descriptors))
                            {
                                FSP_LOG_PRINT("Failed to increment tx semaphore.");
                            }
                        }
                        else
                        {
                            FSP_LOG_PRINT("Failed to get the last NX_PACKET transmitted.");
                        }

                        /* Calculate the index of the next NX_PACKET. */
                        tx_packet_transmitted_index = tx_packet_transmitted_index + 1;
                        if (tx_packet_transmitted_index == p_ether_instance->p_cfg->num_tx_descriptors)
                        {
                            tx_packet_transmitted_index = 0;
                        }
                    } while (p_buffer_current != p_buffer_last);

                    p_netxduo_ether_instance->p_ctrl->tx_packet_transmitted_index = tx_packet_transmitted_index;
                }
            }

#else

            /* Synchronize the IP task with the transmit complete ISR. */
            if (TX_SUCCESS !=
                tx_semaphore_ceiling_put(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore,
                                         p_ether_instance->p_cfg->num_tx_descriptors))
            {
                FSP_LOG_PRINT("Failed to increment tx semaphore.");
            }
#endif

            break;
        }

        case ETHER_EVENT_RX_MESSAGE_LOST:
        case ETHER_EVENT_RX_COMPLETE:
        {
#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT
            rm_netxduo_ether_receive_packet(p_netxduo_ether_instance);
#else
            if (TX_SUCCESS !=
                tx_semaphore_put(&p_netxduo_ether_instance->p_ctrl->ether_rx_semaphore))
            {
                FSP_LOG_PRINT("Failed to increment rx semaphore.");
            }
#endif
            break;
        }

        case ETHER_EVENT_LINK_ON:
        {
#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

            /* Disable Ethernet IRQs so that the receive buffer descriptor can be initialized without being interrupted. */
            R_BSP_IrqDisable(p_ether_instance->p_cfg->irq);

            /* Allocate NetX Packets required for receiving data. */
            NX_PACKET ** p_rx_buffers = p_netxduo_ether_instance->p_cfg->p_rx_packets;
            for (uint32_t i = 0; i < p_ether_instance->p_cfg->num_rx_descriptors; i++)
            {
                if (NX_SUCCESS !=
                    nx_packet_allocate(p_netxduo_ether_instance->p_ctrl->p_ip->nx_ip_default_packet_pool,
                                       &p_rx_buffers[i], NX_RECEIVE_PACKET, TX_NO_WAIT))
                {
                    FSP_LOG_PRINT("Failed to allocate NetX Packet.");
                }

 #if BSP_PERIPHERAL_ESWM_PRESENT

                /*
                 * RMAC doesn't have specific alignment requirements but doesn't support inserting padding so instead
                 * align the ether packet to 4 bytes + 2 so that the IP header will always be 4-byte aligned.
                 */
                p_rx_buffers[i]->nx_packet_prepend_ptr =
                    (UCHAR *) ((((uint32_t) p_rx_buffers[i]->nx_packet_prepend_ptr + 3U) & ~(3U)) + 2);
 #else

                /*
                 * Make sure that the buffer is 32 byte aligned (See "Receive descriptor"
                 * in the EDMAC section of the relevant hardware manual)
                 */
                p_rx_buffers[i]->nx_packet_prepend_ptr =
                    (UCHAR *) (((uint32_t) p_rx_buffers[i]->nx_packet_prepend_ptr + 31U) & ~(31U));
 #endif

                /* Update the buffer pointer in the buffer descriptor. */
                if (FSP_SUCCESS !=
                    p_ether_instance->p_api->rxBufferUpdate(p_ether_instance->p_ctrl,
                                                            p_rx_buffers[i]->nx_packet_prepend_ptr))
                {
                    FSP_LOG_PRINT("Failed to update buffer in r_ether driver.");
                }
            }
            R_BSP_IrqEnable(p_ether_instance->p_cfg->irq);
#endif

            /* Notify NetX that the link is up. */
            p_netxduo_ether_instance->p_ctrl->p_interface->nx_interface_link_up = NX_TRUE;
            _nx_ip_driver_link_status_event(p_netxduo_ether_instance->p_ctrl->p_ip,
                                            p_netxduo_ether_instance->p_ctrl->p_interface->nx_interface_index);
            break;
        }

        case ETHER_EVENT_LINK_OFF:
        {
#if RM_NETXDUO_ETHER_ZERO_COPY_SUPPORT

            /*
             * When the link is re-established, the Ethernet driver will reset all of the buffer descriptors.
             * Release NetX Packets (New packets will be allocated when the link is up).
             */
            p_netxduo_ether_instance->p_ctrl->tx_packet_index             = 0;
            p_netxduo_ether_instance->p_ctrl->tx_packet_transmitted_index = 0;
            NX_PACKET ** p_tx_buffers = p_netxduo_ether_instance->p_cfg->p_tx_packets;
            for (uint32_t i = 0; i < p_ether_instance->p_cfg->num_tx_descriptors; i++)
            {
                if (NULL != p_tx_buffers[i])
                {
                    if (NX_SUCCESS != nx_packet_release(p_tx_buffers[i]))
                    {
                        FSP_LOG_PRINT("Failed to release NetX Packet.");
                    }

                    if (NX_SUCCESS !=
                        tx_semaphore_ceiling_put(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore,
                                                 p_ether_instance->p_cfg->num_tx_descriptors))
                    {
                        FSP_LOG_PRINT("Failed to increment tx semaphore.");
                    }

                    p_tx_buffers[i] = NULL;
                }
            }

            p_netxduo_ether_instance->p_ctrl->rx_packet_index = 0;
            NX_PACKET ** p_rx_buffers = p_netxduo_ether_instance->p_cfg->p_rx_packets;
            for (uint32_t i = 0; i < p_ether_instance->p_cfg->num_rx_descriptors; i++)
            {
                if (NX_SUCCESS != nx_packet_release(p_rx_buffers[i]))
                {
                    FSP_LOG_PRINT("Failed to release NetX Packet.");
                }
            }

#else
            while (TX_CEILING_EXCEEDED !=
                   tx_semaphore_ceiling_put(&p_netxduo_ether_instance->p_ctrl->ether_tx_semaphore,
                                            p_ether_instance->p_cfg->num_tx_descriptors))
            {
                ;
            }
#endif

            /* Notify NetX that the link is down. */
            p_netxduo_ether_instance->p_ctrl->p_interface->nx_interface_link_up = NX_FALSE;
            _nx_ip_driver_link_status_event(p_netxduo_ether_instance->p_ctrl->p_ip,
                                            p_netxduo_ether_instance->p_ctrl->p_interface->nx_interface_index);
            break;
        }

        default:
        {
            break;
        }
    }
}
