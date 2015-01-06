// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

var assertEq = chrome.test.assertEq;
var assertFalse = chrome.test.assertFalse;
var assertTrue = chrome.test.assertTrue;

var EventType = chrome.automation.EventType;
var RoleType = chrome.automation.RoleType;
var StateType = chrome.automation.StateType;

var rootNode = null;

function createTab(url, callback) {
  chrome.tabs.create({"url": url}, function(tab) {
    callback(tab);
  });
}

function setUpAndRunTests(allTests) {
  getUrlFromConfig(function(url) {
    createTab(url, function(unused_tab) {
      chrome.automation.getTree(function (returnedRootNode) {
        rootNode = returnedRootNode;
        if (rootNode.attributes.docLoaded) {
          console.log('In getTree() callback: ' +
                      'docLoaded attr already true; running tests');
          chrome.test.runTests(allTests);
          return;
        }
        console.log('In getTree() callback: ' +
                    'docLoaded false; waiting for loadComplete');
        rootNode.addEventListener('loadComplete', function() {
          console.log('loadComplete received; running tests.');
          chrome.test.runTests(allTests);
        });
      });
    });
  });
}

function getUrlFromConfig(callback) {
  chrome.test.getConfig(function(config) {
    assertTrue('testServer' in config, 'Expected testServer in config');
    var url = 'http://a.com:PORT/index.html'
        .replace(/PORT/, config.testServer.port);
    callback(url)
  });
}
