// treemap-test.js
describe('webtreemapRender', () => {
  let container, node, options;

  beforeEach(() => {
    container = document.createElement('div');
    node = {
      size: 100,
      children: [
        { size: 50 },
        { size: 30 },
        { size: 20 },
      ],
    };
    options = {
      padding: [14, 3, 3, 3],
      lowerBound: 0.1,
      applyMutations: jest.fn(),
      caption: jest.fn(),
      showNode: jest.fn(),
      showChildren: jest.fn(),
    };
  });

  test('renders the tree map', () => {
    webtreemapRender(container, node, options);
    expect(container.querySelector('.webtreemap-node')).toBeTruthy();
  });

  test('applies mutations to nodes', () => {
    webtreemapRender(container, node, options);
    expect(options.applyMutations).toHaveBeenCalledTimes(4);
  });

  test('calls caption function for each node', () => {
    webtreemapRender(container, node, options);
    expect(options.caption).toHaveBeenCalledTimes(4);
  });

  test('calls showNode function for each node', () => {
    webtreemapRender(container, node, options);
    expect(options.showNode).toHaveBeenCalledTimes(4);
  });

  test('calls showChildren function for each node', () => {
    webtreemapRender(container, node, options);
    expect(options.showChildren).toHaveBeenCalledTimes(4);
  });
});
