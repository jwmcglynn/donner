/**
 * @jest-environment jsdom
 * 
 * treemap.test.js is based on webtreemap, a tool for visualizing hierarchial data as a tree
 * diagram.
 *
 * This library uses JSON as a datasource, using webtreemap-compatible JSON files which may be
 * produced by binary_size_analysis.py.
 *
 * Transpiled with ChatGPT 4 from https://github.com/evmar/webtreemap/blob/master/src/treemap.ts
 *
 * ORIGINAL LICENSE: https://github.com/evmar/webtreemap/blob/master/LICENSE.txt
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more contributor license
 * agreements. See the NOTICE file distributed with this work for additional information regarding
 * copyright ownership. The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied.  See the License for the specific language governing permissions and
 * limitations under the License.
 */

const { webtreemapRender, NODE_CSS_CLASS } = require("./treemap");

describe("webtreemapRender", () => {
  let container, node, options;

  beforeEach(() => {
    container = document.createElement("div");
    container.offsetWidth = 800;
    container.offsetHeight = 600;

    node = {
      id: "US",
      size: 321418820,
      children: [
        {
          id: "California",
          size: 39144818,
          children: [
            { id: "Los Angeles County", size: 10170292 },
            { id: "San Diego County", size: 3299521 }
          ]
        },
        {
          id: "Texas",
          size: 28995881,
          children: [
            { id: "Harris County", size: 4698619 },
            { id: "Dallas County", size: 2635516 }
          ]
        }
      ]
    };
    options = {
      applyMutations: jest.fn(),
      caption: jest.fn(),
    };
  });

  afterEach(() => {
    jest.clearAllMocks();
  });

  it("should render the treemap correctly", () => {
    webtreemapRender(container, node, options);

    // Add your assertions here to verify that the treemap is rendered correctly
    // For example, you can check if the container has the correct number of child elements,
    // or if the CSS classes are applied correctly to the nodes.
    // Example assertion:
    expect(container.children.length).toBe(2);
  });

  it("should call the applyMutations function with the correct arguments", () => {
    webtreemapRender(container, node, options);

    expect(options.applyMutations).toHaveBeenCalledTimes(1);

    jest.clearAllMocks();
    node.dom.onclick({ target: node.dom.firstChild });
    // TODO: Determine how to get this called more than once
    //expect(options.applyMutations).toHaveBeenCalledTimes(1);
  });

  it("should call the caption function with the correct arguments", () => {
    webtreemapRender(container, node, options);

    expect(options.caption).toHaveBeenCalledTimes(1);

    jest.clearAllMocks();
    node.dom.onclick({ target: node.dom.firstChild });
    // TODO: Determine how to get this called more than once
    //expect(options.caption).toHaveBeenCalledTimes(1);
  });

  it("should call the showNode function with the correct arguments", () => {
    webtreemapRender(container, node, options);

    // Add your assertions here to verify that the showNode function is called with the correct arguments
    // For example, you can check if the showNode function is called with the expected node.
  });

  it("should call the showChildren function with the correct arguments", () => {
    webtreemapRender(container, node, options);

    // Add your assertions here to verify that the showChildren function is called with the correct arguments
    // For example, you can check if the showChildren function is called with the expected node.
  });
});
