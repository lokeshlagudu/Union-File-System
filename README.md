Union-File-System
=================

Purpose:
========

To build a new stackable file system which supports union operations using stacking technologies.

Introduction:
============

In a stackable file system, each VFS-based object at the stackable file system (e.g., in Wrapfs) has a link to one other
object on the lower file system (sometimes called the "hidden" object). We identify this symbolically as X->X' where "X"
is an object at the upper layer, and X' is an object on the lower layer. This form of stacking is a single-layer linear 
stacking. In a "fan-out" stackable file system, each VFS object X points to two or more objects below: X -> (X1', X2',..
.., Xn'). Fan-out file systems can essentially access two or more "branches" below. This can be used to produce interesting
file systems: replication, fail-over,load-balancing, unification, and more.
