"""
Parses binary size information from bloaty, producing an HTML report that embeds a
webtreemap-compatible JSON blob of the binary size.

This file is a python transpilation of parts of webtreemap:
- https://github.com/evmar/webtreemap/blob/master/src/tree.ts
- https://github.com/evmar/webtreemap/blob/master/src/cli.ts

Transpiled with ChatGPT 4.

ORIGINAL LICENSE: https://github.com/evmar/webtreemap/blob/master/LICENSE.txt
"""

#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

import csv
import sys
import json
import os

from typing import List, Optional, Tuple, Union


class Node:
    """
    Node is the expected shape of input data.
    """

    def __init__(self, id: Optional[str] = None, size: int = 0):
        """
        id is optional but can be used to identify each node.
        It should be unique among nodes at the same level.
        size should be >= the sum of the children's size.
        """
        self.id = id
        self.size = size
        self.children: List[Node] = []

    def find_child(self, id: str) -> Optional["Node"]:
        """
        Find a child node by id.
        """
        for child in self.children:
            if child.id == id:
                return child
        return None

    def add_child(self, child: "Node") -> None:
        """
        Add a child node.
        """
        self.children.append(child)

    def to_dict(self):
        """
        Convert the Node and its children recursively into a dictionary.
        """
        node_dict = {
            "id": self.id,
            "size": self.size,
        }

        if self.children:
            node_dict["children"] = [child.to_dict() for child in self.children]

        return node_dict

    def flatten(self, join=lambda parent, child: f"{parent}/{child}"):
        """
        Flatten nodes that have only one child.
        """
        if self.children:
            for c in self.children:
                c.flatten(join)
            if len(self.children) == 1:
                child = self.children[0]
                self.id = join(self.id, child.id)
                self.children = child.children

    def rollup(self):
        """
        Fill in the size attribute for nodes by summing their children.
        """
        if not self.children:
            return
        total = 0
        for c in self.children:
            c.rollup()
            total += c.size
        if total > self.size:
            self.size = total

    def sort(self):
        """
        Sort a tree by size, descending.
        """
        if not self.children:
            return
        for c in self.children:
            c.sort()
        self.children.sort(key=lambda x: x.size, reverse=True)


def node_to_json(node: Node) -> str:
    """
    Convert a Node instance to a JSON string.
    """
    return json.dumps(node.to_dict(), indent=2)


def treeify(data: List[Tuple[str, int]]) -> Node:
    """
    treeify converts an array of [path, size] pairs into a tree.
    Paths are /-delimited ids.
    """
    tree = Node(size=0)
    for path, size in data:
        parts = path.strip("/").split("/")
        t = tree
        while parts:
            id = parts.pop(0)
            if not t.children:
                t.children = []
            child = t.find_child(id)
            if not child:
                child = Node(id=id, size=0)
                t.add_child(child)
            if not parts:
                child.size += size
            t = child
    return tree


def read_bloaty_csv(filename: str) -> Node:
    data = []
    with open(filename, "r") as file:
        reader = csv.DictReader(file, delimiter=",")
        for row in reader:
            compileunits = row["compileunits"]
            symbols = row["symbols"].replace("::", "/") if "symbols" in row else None
            filesize = int(row["filesize"])
            if compileunits:
                path = f"{compileunits}/{symbols}" if symbols else compileunits
            else:
                path = row["donner_package"]

            data.append((path, filesize))

    return treeify(data)


def generate_webtreemap_html(filename: str, output_filename: str):
    tree = read_bloaty_csv(filename)

    # If there's a common empty parent, skip it.
    if tree.id is None and tree.children and len(tree.children) == 1:
        tree = tree.children[0]

    # If there's an empty parent, roll up for it.
    if tree.size == 0 and tree.children:
        for c in tree.children:
            tree.size += c.size

    tree.rollup()
    tree.sort()
    tree.flatten()

    # Reading the JavaScript file
    script_dir = os.path.dirname(os.path.dirname(__file__))
    with open(os.path.join(script_dir, "treemap.js"), "r") as file:
        treemap_js = file.read()

    # Generating the HTML content
    output = f"""<!doctype html>
<title>webtreemap</title>
<style>
body {{
  font-family: sans-serif;
}}
#treemap {{
  width: auto;
  height: 100%;
}}
</style>
<div id='treemap'></div>
<script>const data = {node_to_json(tree)}</script>
<script>{treemap_js}</script>
<script>webtreemapRender(document.getElementById("treemap"), data, {{
  caption: humanSizeCaption,
}});</script>
"""

    with open(output_filename, "w", encoding="utf-8") as file:
        file.write(output)
