/* virt-p2v
 * Copyright (C) 2009-2019 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * Create the F<physical.xml> file, which is a piece of phony libvirt
 * XML used to communicate the metadata of the physical machine to
 * virt-v2v.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <error.h>

#include <libxml/xmlwriter.h>

#include "getprogname.h"

#include "libxml2-writer-macros.h"

#include "p2v.h"

/* This macro is used by the macros in "libxml2-writer-macros.h"
 * when an error occurs.
 */
#define xml_error(fn)                                           \
  error (EXIT_FAILURE, errno,                                   \
         "%s:%d: error constructing XML near call to \"%s\"",   \
         __FILE__, __LINE__, (fn));

static const char *map_interface_to_network (struct config *, const char *interface);

/**
 * Write the libvirt XML for this physical machine.
 *
 * Note this is not actually input for libvirt.  It's input for
 * virt-v2v on the conversion server.  Virt-v2v will (if necessary)
 * generate the final libvirt XML.
 */
void
generate_physical_xml (struct config *config, struct data_conn *data_conns,
                       const char *filename)
{
  uint64_t memkb;
  CLEANUP_XMLFREETEXTWRITER xmlTextWriterPtr xo = NULL;
  size_t i;

  xo = xmlNewTextWriterFilename (filename, 0);
  if (xo == NULL)
    error (EXIT_FAILURE, errno, "xmlNewTextWriterFilename");

  if (xmlTextWriterSetIndent (xo, 1) == -1 ||
      xmlTextWriterSetIndentString (xo, BAD_CAST "  ") == -1)
    error (EXIT_FAILURE, errno, "could not set XML indent");
  if (xmlTextWriterStartDocument (xo, NULL, NULL, NULL) == -1)
    error (EXIT_FAILURE, errno, "xmlTextWriterStartDocument");

  memkb = config->memory / 1024;

  comment (" %s %s ", getprogname (), PACKAGE_VERSION_FULL);

  comment
    (" NOTE!\n"
     "\n"
     "  This libvirt XML is generated by the virt-p2v front end, in\n"
     "  order to communicate with the backend virt-v2v process running\n"
     "  on the conversion server.  It is a minimal description of the\n"
     "  physical machine.  If the target of the conversion is libvirt,\n"
     "  then virt-v2v will generate the real target libvirt XML, which\n"
     "  has only a little to do with the XML in this file.\n"
     "\n"
     "  TL;DR: Don't try to load this XML into libvirt. ");

  start_element ("domain") {
    attribute ("type", "physical");

    single_element ("name", config->guestname);

    start_element ("memory") {
      attribute ("unit", "KiB");
      string_format ("%" PRIu64, memkb);
    } end_element ();

    start_element ("currentMemory") {
      attribute ("unit", "KiB");
      string_format ("%" PRIu64, memkb);
    } end_element ();

    single_element_format ("vcpu", "%d", config->vcpus);

    if (config->cpu.vendor || config->cpu.model ||
        config->cpu.sockets || config->cpu.cores || config->cpu.threads) {
      /* https://libvirt.org/formatdomain.html#elementsCPU */
      start_element ("cpu") {
        attribute ("match", "minimum");
        if (config->cpu.vendor)
          single_element ("vendor", config->cpu.vendor);
        if (config->cpu.model) {
          start_element ("model") {
            attribute ("fallback", "allow");
            string (config->cpu.model);
          } end_element ();
        }
        if (config->cpu.sockets || config->cpu.cores || config->cpu.threads) {
          start_element ("topology") {
            if (config->cpu.sockets)
              attribute_format ("sockets", "%u", config->cpu.sockets);
            if (config->cpu.cores)
              attribute_format ("cores", "%u", config->cpu.cores);
            if (config->cpu.threads)
              attribute_format ("threads", "%u", config->cpu.threads);
          } end_element ();
        }
      } end_element ();
    }

    switch (config->rtc.basis) {
    case BASIS_UNKNOWN:
      /* Don't emit any <clock> element. */
      break;
    case BASIS_UTC:
      start_element ("clock") {
        if (config->rtc.offset == 0)
          attribute ("offset", "utc");
        else {
          attribute ("offset", "variable");
          attribute ("basis", "utc");
          attribute_format ("adjustment", "%d", config->rtc.offset);
        }
      } end_element ();
      break;
    case BASIS_LOCALTIME:
      start_element ("clock") {
        attribute ("offset", "localtime");
        /* config->rtc.offset is always 0 in this case */
      } end_element ();
      break;
    }

    start_element ("os") {
      start_element ("type") {
        attribute ("arch", host_cpu);
        string ("hvm");
      } end_element ();
    } end_element ();

    start_element ("features") {
      if (config->cpu.acpi) empty_element ("acpi");
      if (config->cpu.apic) empty_element ("apic");
      if (config->cpu.pae)  empty_element ("pae");
    } end_element ();

    start_element ("devices") {

      for (i = 0; config->disks[i] != NULL; ++i) {
        char target_dev[64];

        if (config->disks[i][0] == '/') {
        target_sd:
          memcpy (target_dev, "sd", 2);
          guestfs_int_drive_name (i, &target_dev[2]);
        } else {
          if (strlen (config->disks[i]) <= sizeof (target_dev) - 1)
            strcpy (target_dev, config->disks[i]);
          else
            goto target_sd;
        }

        start_element ("disk") {
          attribute ("type", "network");
          attribute ("device", "disk");
          start_element ("driver") {
            attribute ("name", "qemu");
            attribute ("type", "raw");
          } end_element ();
          start_element ("source") {
            attribute ("protocol", "nbd");
            start_element ("host") {
              attribute ("name", "localhost");
              attribute_format ("port", "%d", data_conns[i].nbd_remote_port);
            } end_element ();
          } end_element ();
          start_element ("target") {
            attribute ("dev", target_dev);
            /* XXX Need to set bus to "ide" or "scsi" here. */
          } end_element ();
        } end_element ();
      }

      if (config->removable) {
        for (i = 0; config->removable[i] != NULL; ++i) {
          start_element ("disk") {
            attribute ("type", "network");
            attribute ("device", "cdrom");
            start_element ("driver") {
              attribute ("name", "qemu");
              attribute ("type", "raw");
            } end_element ();
            start_element ("target") {
              attribute ("dev", config->removable[i]);
            } end_element ();
          } end_element ();
        }
      }

      if (config->interfaces) {
        for (i = 0; config->interfaces[i] != NULL; ++i) {
          const char *target_network;
          CLEANUP_FREE char *mac_filename = NULL;
          CLEANUP_FREE char *mac = NULL;

          target_network =
            map_interface_to_network (config, config->interfaces[i]);

          if (asprintf (&mac_filename, "/sys/class/net/%s/address",
                        config->interfaces[i]) == -1)
            error (EXIT_FAILURE, errno, "asprintf");
          if (g_file_get_contents (mac_filename, &mac, NULL, NULL)) {
            const size_t len = strlen (mac);

            if (len > 0 && mac[len-1] == '\n')
              mac[len-1] = '\0';
          }

          start_element ("interface") {
            attribute ("type", "network");
            start_element ("source") {
              attribute ("network", target_network);
            } end_element ();
            start_element ("target") {
              attribute ("dev", config->interfaces[i]);
            } end_element ();
            if (mac) {
              start_element ("mac") {
                attribute ("address", mac);
              } end_element ();
            }
          } end_element ();
        }
      }

    } end_element (); /* </devices> */

  } end_element (); /* </domain> */

  if (xmlTextWriterEndDocument (xo) == -1)
    error (EXIT_FAILURE, errno, "xmlTextWriterEndDocument");
}

/**
 * Using C<config-E<gt>network_map>, map the interface to a target
 * network name.  If no map is found, return C<default>.  See
 * L<virt-p2v(1)> documentation of C<"p2v.network"> for how the
 * network map works.
 *
 * Note this returns a static string which is only valid as long as
 * C<config-E<gt>network_map> is not freed.
 */
static const char *
map_interface_to_network (struct config *config, const char *interface)
{
  size_t i, len;

  if (config->network_map == NULL)
    return "default";

  for (i = 0; config->network_map[i] != NULL; ++i) {
    /* The default map maps everything. */
    if (strchr (config->network_map[i], ':') == NULL)
      return config->network_map[i];

    /* interface: ? */
    len = strlen (interface);
    if (STRPREFIX (config->network_map[i], interface) &&
        config->network_map[i][len] == ':')
      return &config->network_map[i][len+1];
  }

  /* No mapping found. */
  return "default";
}
