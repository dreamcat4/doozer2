all:
	$(MAKE) -C server
	$(MAKE) -C ctl

clean install uninstall:
	$(MAKE) -C server $@
	$(MAKE) -C ctl $@

