![icon](/data/pixmaps/idbutton.png?raw=true) darktable ![build status](https://travis-ci.org/darktable-org/darktable.svg?branch=master)
=========

darktable is an open source photography workflow application and raw developer. A virtual lighttable and darkroom for photographers. It manages your digital negatives in a database, lets you view them through a zoomable lighttable and enables you to develop raw images and enhance them.

[https://www.darktable.org/](https://www.darktable.org/ "darktable homepage")

Contributing
------------

* Write a blog about darktable
* Create a tutorial for darktable
* Help expand the [user wiki](https://www.darktable.org/redmine/projects/users/wiki)
* Answer questions on the [user mailing list](https://www.mail-archive.com/darktable-user@lists.darktable.org/)
* Share your ideas on the [developer mailing list](https://www.mail-archive.com/darktable-dev@lists.darktable.org/)
* Test [releases](https://www.darktable.org/install/)
* Review [pull requests](https://github.com/darktable-org/darktable/pulls)
* Start [hacking on darktable](https://www.darktable.org/redmine/projects/darktable/wiki/Contributing_code)

Building
--------

### Easy way

```bash
./build.sh --prefix /opt/darktable --build-type Release
```

### Manual way

```bash
mkdir build/
cd build/
cmake -DCMAKE_INSTALL_PREFIX=/opt/darktable/ ..
make
sudo make install
```

### Further reading

There is a [comprehensive list](https://redmine.darktable.org/projects/darktable/wiki/Building_darktable_20) of build instructions for all the widely used Linux distributions.


**Tip:** Check that you have the latest [gphoto2 library](http://www.gphoto.org/ "gphoto2 homepage") installed in order to support the newest cameras.

Issue tracking
--------------

[https://www.darktable.org/redmine/projects/darktable/issues](https://www.darktable.org/redmine/projects/darktable/issues "darktable issue tracking")

Wiki
----

* [User wiki](https://www.darktable.org/redmine/projects/users/wiki "darktable user wiki")
* [Developer wiki](https://www.darktable.org/redmine/projects/darktable/wiki "darktable developer wiki")


Mailing lists
-------------

* Users [[subscribe](mailto:darktable-user+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-user@lists.darktable.org/)]
* Developer [[subscribe](mailto:darktable-dev+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-dev@lists.darktable.org/)]
