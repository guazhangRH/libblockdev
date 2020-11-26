import tempfile
import re
import six

from distutils.version import LooseVersion

from .fs_test import FSTestCase, mounted

import overrides_hack
import utils
from utils import TestTags, tag_test

from gi.repository import BlockDev, GLib


class F2FSTestCase(FSTestCase):
    def setUp(self):
        if not self.f2fs_avail:
            self.skipTest("skipping F2FS: not available")

        super(F2FSTestCase, self).setUp()

        self.mount_dir = tempfile.mkdtemp(prefix="libblockdev.", suffix="f2fs_test")


class F2FSTestMkfs(F2FSTestCase):
    def test_f2fs_mkfs(self):
        """Verify that it is possible to create a new f2fs file system"""

        with self.assertRaises(GLib.GError):
            BlockDev.fs_f2fs_mkfs("/non/existing/device", None)

        succ = BlockDev.fs_f2fs_mkfs(self.loop_dev, None)
        self.assertTrue(succ)

        # just try if we can mount the file system
        with mounted(self.loop_dev, self.mount_dir):
            pass

        # check the fstype
        fstype = BlockDev.fs_get_fstype(self.loop_dev)
        self.assertEqual(fstype, "f2fs")

        BlockDev.fs_wipe(self.loop_dev, True)


class F2FSMkfsWithLabel(F2FSTestCase):
    def test_f2fs_mkfs_with_label(self):
        """Verify that it is possible to create an f2fs file system with label"""

        ea = BlockDev.ExtraArg.new("-l", "TEST_LABEL")
        succ = BlockDev.fs_f2fs_mkfs(self.loop_dev, [ea])
        self.assertTrue(succ)

        fi = BlockDev.fs_f2fs_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.label, "TEST_LABEL")


class F2FSMkfsWithFeatures(F2FSTestCase):
    def test_f2fs_mkfs_with_label(self):
        """Verify that it is possible to create an f2fs file system with extra features enabled"""

        ea = BlockDev.ExtraArg.new("-O", "encrypt")
        succ = BlockDev.fs_f2fs_mkfs(self.loop_dev, [ea])
        self.assertTrue(succ)

        fi = BlockDev.fs_f2fs_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertTrue(fi.features & BlockDev.FSF2FSFeature.ENCRYPT)


class F2FSTestWipe(F2FSTestCase):
    def test_f2fs_wipe(self):
        """Verify that it is possible to wipe an f2fs file system"""

        succ = BlockDev.fs_f2fs_mkfs(self.loop_dev, None)
        self.assertTrue(succ)

        succ = BlockDev.fs_f2fs_wipe(self.loop_dev)
        self.assertTrue(succ)

        # already wiped, should fail this time
        with self.assertRaises(GLib.GError):
            BlockDev.fs_f2fs_wipe(self.loop_dev)

        utils.run("pvcreate -ff -y %s >/dev/null" % self.loop_dev)

        # LVM PV signature, not an f2fs file system
        with self.assertRaises(GLib.GError):
            BlockDev.fs_f2fs_wipe(self.loop_dev)

        BlockDev.fs_wipe(self.loop_dev, True)

        utils.run("mkfs.ext2 -F %s >/dev/null 2>&1" % self.loop_dev)

        # ext2, not an f2fs file system
        with self.assertRaises(GLib.GError):
            BlockDev.fs_f2fs_wipe(self.loop_dev)

        BlockDev.fs_wipe(self.loop_dev, True)


class F2FSTestCheck(F2FSTestCase):
    def _check_fsck_f2fs_version(self):
        # if it can run -V to get version it can do the check
        ret, _out, _err = utils.run_command("fsck.f2fs -V")
        return ret == 0

    def test_f2fs_check(self):
        """Verify that it is possible to check an f2fs file system"""

        succ = BlockDev.fs_f2fs_mkfs(self.loop_dev, None)
        self.assertTrue(succ)

        if not self._check_fsck_f2fs_version():
            with six.assertRaisesRegex(self, GLib.GError, "Too low version of fsck.f2fs. At least 1.11.0 required."):
                BlockDev.fs_f2fs_check(self.loop_dev, None)
        else:
            succ = BlockDev.fs_f2fs_check(self.loop_dev, None)
            self.assertTrue(succ)


class F2FSTestRepair(F2FSTestCase):
    def test_f2fs_repair(self):
        """Verify that it is possible to repair an f2fs file system"""

        succ = BlockDev.fs_f2fs_mkfs(self.loop_dev, None)
        self.assertTrue(succ)

        succ = BlockDev.fs_f2fs_repair(self.loop_dev, None)
        self.assertTrue(succ)


class F2FSGetInfo(F2FSTestCase):
    def test_f2fs_get_info(self):
        """Verify that it is possible to get info about an f2fs file system"""

        succ = BlockDev.fs_f2fs_mkfs(self.loop_dev, None)
        self.assertTrue(succ)

        fi = BlockDev.fs_f2fs_get_info(self.loop_dev)
        self.assertTrue(fi)
        self.assertEqual(fi.label, "")
        # should be an non-empty string
        self.assertTrue(fi.uuid)


class F2FSResize(F2FSTestCase):
    def _can_resize_f2fs(self):
        ret, out, _err = utils.run_command("resize.f2fs -V")
        if ret != 0:
            # we can't even check the version
            return False

        m = re.search(r"resize.f2fs ([\d\.]+)", out)
        if not m or len(m.groups()) != 1:
            raise RuntimeError("Failed to determine f2fs version from: %s" % out)
        return LooseVersion(m.groups()[0]) >= LooseVersion("1.12.0")

    @tag_test(TestTags.UNSTABLE)
    def test_f2fs_resize(self):
        """Verify that it is possible to resize an f2fs file system"""

        succ = BlockDev.fs_f2fs_mkfs(self.loop_dev, None)
        self.assertTrue(succ)

        # shrink without the safe option -- should fail
        with self.assertRaises(GLib.GError):
            BlockDev.fs_f2fs_resize(self.loop_dev, 100 * 1024**2 / 512, False)

        # if we can't shrink we'll just check it returns some sane error
        if not self._can_resize_f2fs():
            with six.assertRaisesRegex(self, GLib.GError, "Too low version of resize.f2fs. At least 1.12.0 required."):
                BlockDev.fs_f2fs_resize(self.loop_dev, 100 * 1024**2 / 512, True)
            return

        succ = BlockDev.fs_f2fs_resize(self.loop_dev, 100 * 1024**2 / 512, True)
        self.assertTrue(succ)

        fi = BlockDev.fs_f2fs_get_info(self.loop_dev)
        self.assertEqual(fi.sector_count * fi.sector_size, 100 * 1024**2)

        # grow
        succ = BlockDev.fs_f2fs_resize(self.loop_dev, 120 * 1024**2 / 512, True)
        self.assertTrue(succ)

        fi = BlockDev.fs_f2fs_get_info(self.loop_dev)
        self.assertEqual(fi.sector_count * fi.sector_size, 120 * 1024**2)

        # shrink again
        succ = BlockDev.fs_f2fs_resize(self.loop_dev, 100 * 1024**2 / 512, True)
        self.assertTrue(succ)

        fi = BlockDev.fs_f2fs_get_info(self.loop_dev)
        self.assertEqual(fi.sector_count * fi.sector_size, 100 * 1024**2)

        # resize to maximum size
        succ = BlockDev.fs_f2fs_resize(self.loop_dev, 0, False)
        self.assertTrue(succ)

        fi = BlockDev.fs_f2fs_get_info(self.loop_dev)
        self.assertEqual(fi.sector_count * fi.sector_size, self.loop_size)
