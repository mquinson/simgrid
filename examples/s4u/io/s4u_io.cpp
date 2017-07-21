/* Copyright (c) 2006-2017. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include <unordered_map>

#include "simgrid/s4u.hpp"

XBT_LOG_NEW_DEFAULT_CATEGORY(s4u_test, "a sample log category");

class MyHost {
public:
  void show_info(std::unordered_map<std::string, simgrid::s4u::Storage*> const& mounts)
  {
    XBT_INFO("Storage info on %s:", simgrid::s4u::Host::current()->getCname());

    for (const auto&kv : mounts) {
      const char* mountpoint = kv.first.c_str();
      simgrid::s4u::Storage* storage = kv.second;

      // Retrieve disk's information
      sg_size_t free_size = storage->getSizeFree();
      sg_size_t used_size = storage->getSizeUsed();
      sg_size_t size      = storage->getSize();

      XBT_INFO("    %s (%s) Used: %llu; Free: %llu; Total: %llu.", storage->getName(), mountpoint, used_size, free_size,
               size);
    }
  }

  void operator()() {
    std::unordered_map<std::string, simgrid::s4u::Storage*> const& mounts =
        simgrid::s4u::Host::current()->getMountedStorages();

    show_info(mounts);

    // Open an non-existing file to create it
    const char* filename = "/home/tmp/data.txt";
    simgrid::s4u::File* file = new simgrid::s4u::File(filename, nullptr);

    sg_size_t write = file->write(200000);  // Write 200,000 bytes
    XBT_INFO("Create a %llu bytes file named '%s' on /sd1", write, filename);

    // check that sizes have changed
    show_info(mounts);

    // Now retrieve the size of created file and read it completely
    const sg_size_t file_size = file->size();
    file->seek(0);
    const sg_size_t read = file->read(file_size);
    XBT_INFO("Read %llu bytes on %s", read, filename);

    // Now write 100,000 bytes in tmp/data.txt
    write = file->write(100000);  // Write 100,000 bytes
    XBT_INFO("Write %llu bytes on %s", write, filename);

    simgrid::s4u::Storage* storage = simgrid::s4u::Storage::byName("Disk4");

    // Now rename file from ./tmp/data.txt to ./tmp/simgrid.readme
    const char *newpath = "/home/tmp/simgrid.readme";
    XBT_INFO("Move '%s' to '%s'", file->getPath(), newpath);
    file->move(newpath);

    // Test attaching some user data to the file
    file->setUserdata(xbt_strdup("777"));
    XBT_INFO("User data attached to the file: %s", (char*)file->getUserdata());
    xbt_free(file->getUserdata());

    // Close the file
    delete file;

    // Now attach some user data to disk1
    XBT_INFO("Get/set data for storage element: %s", storage->getName());
    XBT_INFO("    Uninitialized storage data: '%s'", (char*)storage->getUserdata());

    storage->setUserdata(xbt_strdup("Some user data"));
    XBT_INFO("    Set and get data: '%s'", (char*)storage->getUserdata());

    xbt_free(storage->getUserdata());
  }
};

int main(int argc, char **argv)
{
  simgrid::s4u::Engine *e = new simgrid::s4u::Engine(&argc,argv);
  e->loadPlatform("../../platforms/storage/storage.xml");
  simgrid::s4u::Actor::createActor("host", simgrid::s4u::Host::by_name("denise"), MyHost());
  e->run();
  delete e;
  return 0;
}
