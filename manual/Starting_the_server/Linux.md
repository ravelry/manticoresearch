# Starting Manticore in Linux

When Manticore Search is installed using DEB or RPM packages, the searchd process can be run and managed by operating system's init system. Most Linux versions now use systemd, while older releases use SysV init.

If you are not sure about the type of the init system your platform use, run:

```shell
ps --no-headers -o comm 1
```

### Starting and stopping using systemd

After the installation the Manticore Search service is not started automatically. To start Manticore run the following command:

```shell
sudo systemctl start manticore
```

To stop Manticore run the following command:


```shell
sudo systemctl stop manticore
```

The Manticore service is set to run at boot. You can check it by running:

```shell
sudo systemctl is-active manticore
```

If you want to disable Manticore starting at boot time run:

```shell
sudo systemctl disable manticore
```

To enable Manticore to start at boot, run:

```shell
sudo systemctl enable manticore
```


`searchd` process logs startup information in `systemd` journal. If `systemd` logging is enabled you can view the logged information with the following command:

```shell
sudo journalctl --unit manticore
```
### Starting and stopping using service

Manticore can be started and stopped using service commands:

```shell
sudo service manticore start
sudo service manticore stop
```

To enable the sysV service at boot on RedHat systems run:

```shell
chkconfig manticore on
```

To enable the sysV service at boot on Debian systems (including Ubuntu) run:

```shell
update-rc.d manticore defaults
```

Please note that `searchd` is started by the init system under  `manticore` user and all files created by the server will be owned by this user. If `searchd` is started under ,for example, root user, the permissions of files will be changed which may lead to issues when running again `searchd` as service.