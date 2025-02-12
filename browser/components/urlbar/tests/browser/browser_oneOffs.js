const TEST_ENGINE_BASENAME = "searchSuggestionEngine.xml";

let gMaxResults;
let engine;

XPCOMUtils.defineLazyGetter(this, "oneOffSearchButtons", () => {
  return UrlbarTestUtils.getOneOffSearchButtons(window);
});

add_task(async function init() {
  gMaxResults = Services.prefs.getIntPref("browser.urlbar.maxRichResults");

  // Add a search suggestion engine and move it to the front so that it appears
  // as the first one-off.
  engine = await SearchTestUtils.promiseNewSearchEngine(
    getRootDirectory(gTestPath) + TEST_ENGINE_BASENAME
  );
  await Services.search.moveEngine(engine, 0);

  Services.prefs.setBoolPref(
    "browser.search.separatePrivateDefault.ui.enabled",
    false
  );
  registerCleanupFunction(async function() {
    await PlacesUtils.history.clear();
    await UrlbarTestUtils.formHistory.clear();
    Services.prefs.clearUserPref(
      "browser.search.separatePrivateDefault.ui.enabled"
    );
    Services.prefs.clearUserPref("browser.urlbar.update2");
    Services.prefs.clearUserPref("browser.urlbar.update2.oneOffsRefresh");
  });

  // Initialize history with enough visits to fill up the view.
  await PlacesUtils.history.clear();
  await UrlbarTestUtils.formHistory.clear();
  for (let i = 0; i < gMaxResults; i++) {
    await PlacesTestUtils.addVisits(
      "http://example.com/browser_urlbarOneOffs.js/?" + i
    );
  }

  // Add some more visits to the last URL added above so that the top-sites view
  // will be non-empty.
  for (let i = 0; i < 5; i++) {
    await PlacesTestUtils.addVisits(
      "http://example.com/browser_urlbarOneOffs.js/?" + (gMaxResults - 1)
    );
  }
  await updateTopSites(sites => {
    return sites && sites[0] && sites[0].url.startsWith("http://example.com/");
  });
});

// Opens the top-sites view, i.e., the view that's shown when the input hasn't
// been edited.  The one-offs should be hidden.
add_task(async function topSitesView() {
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "",
    fireInputEvent: true,
  });
  Assert.equal(
    UrlbarTestUtils.getOneOffSearchButtonsVisible(window),
    false,
    "One-offs should be hidden"
  );
  await hidePopup();
});

// Opens the top-sites view with update2 enabled.  The one-offs should be shown.
add_task(async function topSitesViewUpdate2() {
  // Set the update2 prefs.
  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.urlbar.update2", true],
      ["browser.urlbar.update2.oneOffsRefresh", true],
    ],
  });

  // Do a search.
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "",
    fireInputEvent: true,
  });
  await TestUtils.waitForCondition(
    () => !oneOffSearchButtons._rebuilding,
    "Waiting for one-offs to finish rebuilding"
  );

  // There's one top sites result, the page with a lot of visits from init.
  let resultURL = "example.com/browser_urlbarOneOffs.js/?" + (gMaxResults - 1);
  Assert.equal(UrlbarTestUtils.getResultCount(window), 1, "Result count");

  Assert.equal(
    UrlbarTestUtils.getOneOffSearchButtonsVisible(window),
    true,
    "One-offs are visible"
  );

  assertState(-1, -1, "");

  // Key down into the result.
  EventUtils.synthesizeKey("KEY_ArrowDown");
  assertState(0, -1, resultURL);

  // Key down through each one-off.
  let numButtons = oneOffSearchButtons.getSelectableButtons(true).length;
  for (let i = 0; i < numButtons; i++) {
    EventUtils.synthesizeKey("KEY_ArrowDown");
    assertState(-1, i, "");
  }

  // Key down again.  The selection should go away.
  EventUtils.synthesizeKey("KEY_ArrowDown");
  assertState(-1, -1, "");

  // Key down again.  The result should be selected.
  EventUtils.synthesizeKey("KEY_ArrowDown");
  assertState(0, -1, resultURL);

  // Key back up.  The selection should go away.
  EventUtils.synthesizeKey("KEY_ArrowUp");
  assertState(-1, -1, "");

  // Key up again.  The selection should wrap back around to the one-offs.  Key
  // up through all the one-offs.
  for (let i = numButtons - 1; i >= 0; i--) {
    EventUtils.synthesizeKey("KEY_ArrowUp");
    assertState(-1, i, "");
  }

  // Key up.  The result should be selected.
  EventUtils.synthesizeKey("KEY_ArrowUp");
  assertState(0, -1, resultURL);

  // Key up again.  The selection should go away.
  EventUtils.synthesizeKey("KEY_ArrowUp");
  assertState(-1, -1, "");

  await hidePopup();
  await SpecialPowers.popPrefEnv();
});

// Keys up and down through the non-top-sites view, i.e., the view that's shown
// when the input has been edited.
add_task(async function editedView() {
  // Use a typed value that returns the visits added above but that doesn't
  // trigger autofill since that would complicate the test.
  let typedValue = "browser_urlbarOneOffs";
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: typedValue,
    fireInputEvent: true,
  });
  await UrlbarTestUtils.waitForAutocompleteResultAt(window, gMaxResults - 1);
  let heuristicResult = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  assertState(0, -1, typedValue);

  // Key down through each result.  The first result is already selected, which
  // is why gMaxResults - 1 is the correct number of times to do this.
  for (let i = 0; i < gMaxResults - 1; i++) {
    EventUtils.synthesizeKey("KEY_ArrowDown");
    // i starts at zero so that the textValue passed to assertState is correct.
    // But that means that i + 1 is the expected selected index, since initially
    // (when this loop starts) the first result is selected.
    assertState(
      i + 1,
      -1,
      "example.com/browser_urlbarOneOffs.js/?" + (gMaxResults - i - 1)
    );
    Assert.ok(
      !BrowserTestUtils.is_visible(heuristicResult.element.action),
      "The heuristic action should not be visible"
    );
  }

  // Key down through each one-off.
  let numButtons = oneOffSearchButtons.getSelectableButtons(true).length;
  for (let i = 0; i < numButtons; i++) {
    EventUtils.synthesizeKey("KEY_ArrowDown");
    assertState(-1, i, typedValue);
    Assert.equal(
      BrowserTestUtils.is_visible(heuristicResult.element.action),
      !oneOffSearchButtons.selectedButton.classList.contains(
        "search-setting-button-compact"
      ),
      "The heuristic action should be visible when a one-off button is selected"
    );
  }

  // Key down once more.  The selection should wrap around to the first result.
  EventUtils.synthesizeKey("KEY_ArrowDown");
  assertState(0, -1, typedValue);
  Assert.ok(
    BrowserTestUtils.is_visible(heuristicResult.element.action),
    "The heuristic action should be visible"
  );

  // Now key up.  The selection should wrap back around to the one-offs.  Key
  // up through all the one-offs.
  for (let i = numButtons - 1; i >= 0; i--) {
    EventUtils.synthesizeKey("KEY_ArrowUp");
    assertState(-1, i, typedValue);
    Assert.equal(
      BrowserTestUtils.is_visible(heuristicResult.element.action),
      !oneOffSearchButtons.selectedButton.classList.contains(
        "search-setting-button-compact"
      ),
      "The heuristic action should be visible when a one-off button is selected"
    );
  }

  // Key up through each non-heuristic result.
  for (let i = gMaxResults - 2; i >= 0; i--) {
    EventUtils.synthesizeKey("KEY_ArrowUp");
    assertState(
      i + 1,
      -1,
      "example.com/browser_urlbarOneOffs.js/?" + (gMaxResults - i - 1)
    );
    Assert.ok(
      !BrowserTestUtils.is_visible(heuristicResult.element.action),
      "The heuristic action should not be visible"
    );
  }

  // Key up once more.  The heuristic result should be selected.
  EventUtils.synthesizeKey("KEY_ArrowUp");
  assertState(0, -1, typedValue);
  Assert.ok(
    BrowserTestUtils.is_visible(heuristicResult.element.action),
    "The heuristic action should be visible"
  );

  await hidePopup();
});

// Checks that "Search with Current Search Engine" items are updated to "Search
// with One-Off Engine" when a one-off is selected. If update2 one-offs are
// enabled, only the heuristic result should update.
add_task(async function searchWith() {
  // Enable suggestions for this subtest so we can check non-heuristic results.
  let oldDefaultEngine = await Services.search.getDefault();
  let oldSuggestPref = Services.prefs.getBoolPref(
    "browser.urlbar.suggest.searches"
  );
  await Services.search.setDefault(engine);
  Services.prefs.setBoolPref("browser.urlbar.suggest.searches", true);

  let typedValue = "foo";
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: typedValue,
  });
  let result = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  assertState(0, -1, typedValue);

  Assert.equal(
    result.displayed.action,
    "Search with " + (await Services.search.getDefault()).name,
    "Sanity check: first result's action text"
  );

  // Alt+Down to the second one-off.  Now the first result and the second
  // one-off should both be selected.
  EventUtils.synthesizeKey("KEY_ArrowDown", { altKey: true, repeat: 2 });
  assertState(0, 1, typedValue);

  let engineName = oneOffSearchButtons.selectedButton.engine.name;
  Assert.notEqual(
    engineName,
    (await Services.search.getDefault()).name,
    "Sanity check: Second one-off engine should not be the current engine"
  );
  result = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  Assert.equal(
    result.displayed.action,
    "Search with " + engineName,
    "First result's action text should be updated"
  );

  // Check non-heuristic results.
  for (let refresh of [true, false]) {
    UrlbarPrefs.set("update2", refresh);
    UrlbarPrefs.set("update2.oneOffsRefresh", refresh);
    await hidePopup();
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: typedValue,
    });

    EventUtils.synthesizeKey("KEY_ArrowDown");
    result = await UrlbarTestUtils.getDetailsOfResultAt(window, 1);
    assertState(1, -1, typedValue + "foo");
    Assert.equal(
      result.displayed.action,
      "Search with " + engine.name,
      "Sanity check: second result's action text"
    );
    Assert.ok(!result.heuristic, "The second result is not heuristic.");
    EventUtils.synthesizeKey("KEY_ArrowDown", { altKey: true, repeat: 2 });
    assertState(1, 1, typedValue + "foo");

    engineName = oneOffSearchButtons.selectedButton.engine.name;
    Assert.notEqual(
      engineName,
      (await Services.search.getDefault()).name,
      "Sanity check: Second one-off engine should not be the current engine"
    );
    result = await UrlbarTestUtils.getDetailsOfResultAt(window, 1);

    if (refresh) {
      Assert.equal(
        result.displayed.action,
        "Search with " + (await Services.search.getDefault()).name,
        "Second result's action text should be the same"
      );
    } else {
      Assert.equal(
        result.displayed.action,
        "Search with " + engineName,
        "Second result's action text should be updated"
      );
    }
  }

  Services.prefs.setBoolPref("browser.urlbar.suggest.searches", oldSuggestPref);
  await Services.search.setDefault(oldDefaultEngine);
  await hidePopup();
});

// Clicks a one-off.
add_task(async function oneOffClick() {
  gBrowser.selectedTab = BrowserTestUtils.addTab(gBrowser);

  // We are explicitly using something that looks like a url, to make the test
  // stricter. Even if it looks like a url, we should search.
  let typedValue = "foo.bar";

  for (let refresh of [true, false]) {
    UrlbarPrefs.set("update2", refresh);
    UrlbarPrefs.set("update2.oneOffsRefresh", refresh);

    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: typedValue,
    });
    await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
    assertState(0, -1, typedValue);
    let oneOffs = oneOffSearchButtons.getSelectableButtons(true);

    if (refresh) {
      EventUtils.synthesizeMouseAtCenter(oneOffs[0], {});
      Assert.ok(
        UrlbarTestUtils.isPopupOpen(window),
        "Urlbar view is still open."
      );
      Assert.ok(
        UrlbarTestUtils.isInSearchMode(window, oneOffs[0].engine.name),
        "The Urlbar is in search mode."
      );
      window.gURLBar.setSearchMode(null);
    } else {
      let resultsPromise = BrowserTestUtils.browserLoaded(
        gBrowser.selectedBrowser,
        false,
        "http://mochi.test:8888/?terms=foo.bar"
      );
      EventUtils.synthesizeMouseAtCenter(oneOffs[0], {});
      await resultsPromise;
    }
  }

  gBrowser.removeTab(gBrowser.selectedTab);
  await UrlbarTestUtils.formHistory.clear();
});

// Presses the Return key when a one-off is selected.
add_task(async function oneOffReturn() {
  gBrowser.selectedTab = BrowserTestUtils.addTab(gBrowser);

  // We are explicitly using something that looks like a url, to make the test
  // stricter. Even if it looks like a url, we should search.
  let typedValue = "foo.bar";

  for (let refresh of [true, false]) {
    UrlbarPrefs.set("update2", refresh);
    UrlbarPrefs.set("update2.oneOffsRefresh", refresh);

    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: typedValue,
      fireInputEvent: true,
    });
    await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
    assertState(0, -1, typedValue);
    let oneOffs = oneOffSearchButtons.getSelectableButtons(true);

    // Alt+Down to select the first one-off.
    EventUtils.synthesizeKey("KEY_ArrowDown", { altKey: true });
    assertState(0, 0, typedValue);

    if (refresh) {
      EventUtils.synthesizeKey("KEY_Enter");
      Assert.ok(
        UrlbarTestUtils.isPopupOpen(window),
        "Urlbar view is still open."
      );
      Assert.ok(
        UrlbarTestUtils.isInSearchMode(window, oneOffs[0].engine.name),
        "The Urlbar is in search mode."
      );
      window.gURLBar.setSearchMode(null);
    } else {
      let resultsPromise = BrowserTestUtils.browserLoaded(
        gBrowser.selectedBrowser,
        false,
        "http://mochi.test:8888/?terms=foo.bar"
      );
      EventUtils.synthesizeKey("KEY_Enter");
      await resultsPromise;
    }
  }

  gBrowser.removeTab(gBrowser.selectedTab);
  await UrlbarTestUtils.formHistory.clear();
});

add_task(async function hiddenOneOffs() {
  // Disable all the engines but the current one, check the oneoffs are
  // hidden and that moving up selects the last match.
  let defaultEngine = await Services.search.getDefault();
  let engines = (await Services.search.getVisibleEngines()).filter(
    e => e.name != defaultEngine.name
  );
  await SpecialPowers.pushPrefEnv({
    set: [["browser.search.hiddenOneOffs", engines.map(e => e.name).join(",")]],
  });

  let typedValue = "foo";
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: typedValue,
    fireInputEvent: true,
  });
  await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  assertState(0, -1);
  Assert.equal(
    getComputedStyle(oneOffSearchButtons.container).display,
    "none",
    "The one-off buttons should be hidden"
  );
  EventUtils.synthesizeKey("KEY_ArrowUp");
  assertState(1, -1);
  await hidePopup();
});

// The one-offs should be hidden when searching with an "@engine" search engine
// alias.
add_task(async function hiddenWhenUsingSearchAlias() {
  let typedValue = "@example";
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: typedValue,
    fireInputEvent: true,
  });
  await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  Assert.equal(
    UrlbarTestUtils.getOneOffSearchButtonsVisible(window),
    false,
    "Should not be showing the one-off buttons"
  );
  await hidePopup();

  typedValue = "not an engine alias";
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: typedValue,
    fireInputEvent: true,
  });
  await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  Assert.equal(
    UrlbarTestUtils.getOneOffSearchButtonsVisible(window),
    true,
    "Should be showing the one-off buttons"
  );
  await hidePopup();
});

function assertState(result, oneOff, textValue = undefined) {
  Assert.equal(
    UrlbarTestUtils.getSelectedRowIndex(window),
    result,
    "Expected result should be selected"
  );
  Assert.equal(
    oneOffSearchButtons.selectedButtonIndex,
    oneOff,
    "Expected one-off should be selected"
  );
  if (textValue !== undefined) {
    Assert.equal(gURLBar.value, textValue, "Expected value");
  }
}

function hidePopup() {
  return UrlbarTestUtils.promisePopupClose(window, () => {
    EventUtils.synthesizeKey("KEY_Escape");
  });
}
