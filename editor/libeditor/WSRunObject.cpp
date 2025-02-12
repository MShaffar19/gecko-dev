/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WSRunObject.h"

#include "HTMLEditUtils.h"

#include "mozilla/Assertions.h"
#include "mozilla/Casting.h"
#include "mozilla/EditorDOMPoint.h"
#include "mozilla/HTMLEditor.h"
#include "mozilla/mozalloc.h"
#include "mozilla/OwningNonNull.h"
#include "mozilla/RangeUtils.h"
#include "mozilla/SelectionState.h"
#include "mozilla/StaticPrefs_dom.h"     // for StaticPrefs::dom_*
#include "mozilla/StaticPrefs_editor.h"  // for StaticPrefs::editor_*
#include "mozilla/InternalMutationEvent.h"
#include "mozilla/dom/AncestorIterator.h"

#include "nsAString.h"
#include "nsCRT.h"
#include "nsContentUtils.h"
#include "nsDebug.h"
#include "nsError.h"
#include "nsIContent.h"
#include "nsIContentInlines.h"
#include "nsISupportsImpl.h"
#include "nsRange.h"
#include "nsString.h"
#include "nsTextFragment.h"

namespace mozilla {

using namespace dom;

using ChildBlockBoundary = HTMLEditUtils::ChildBlockBoundary;

const char16_t kNBSP = 160;

template WSScanResult WSRunScanner::ScanPreviousVisibleNodeOrBlockBoundaryFrom(
    const EditorDOMPoint& aPoint) const;
template WSScanResult WSRunScanner::ScanPreviousVisibleNodeOrBlockBoundaryFrom(
    const EditorRawDOMPoint& aPoint) const;
template WSScanResult WSRunScanner::ScanNextVisibleNodeOrBlockBoundaryFrom(
    const EditorDOMPoint& aPoint) const;
template WSScanResult WSRunScanner::ScanNextVisibleNodeOrBlockBoundaryFrom(
    const EditorRawDOMPoint& aPoint) const;

template nsresult WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt(
    HTMLEditor& aHTMLEditor, const EditorDOMPoint& aScanStartPoint);
template nsresult WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt(
    HTMLEditor& aHTMLEditor, const EditorRawDOMPoint& aScanStartPoint);
template nsresult WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt(
    HTMLEditor& aHTMLEditor, const EditorDOMPointInText& aScanStartPoint);

template WSRunScanner::TextFragmentData::TextFragmentData(
    const EditorDOMPoint& aPoint, const Element* aEditingHost);
template WSRunScanner::TextFragmentData::TextFragmentData(
    const EditorRawDOMPoint& aPoint, const Element* aEditingHost);
template WSRunScanner::TextFragmentData::TextFragmentData(
    const EditorDOMPointInText& aPoint, const Element* aEditingHost);

// static
nsresult WhiteSpaceVisibilityKeeper::PrepareToJoinBlocks(
    HTMLEditor& aHTMLEditor, Element& aLeftBlockElement,
    Element& aRightBlockElement) {
  nsresult rv = WhiteSpaceVisibilityKeeper::
      MakeSureToKeepVisibleStateOfWhiteSpacesAroundDeletingRange(
          aHTMLEditor,
          EditorDOMRange(EditorRawDOMPoint::AtEndOf(aLeftBlockElement),
                         EditorRawDOMPoint(&aRightBlockElement, 0)));
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "WhiteSpaceVisibilityKeeper::"
      "MakeSureToKeepVisibleStateOfWhiteSpacesAroundDeletingRange() failed");
  return rv;
}

nsresult WhiteSpaceVisibilityKeeper::PrepareToDeleteRange(
    HTMLEditor& aHTMLEditor, EditorDOMPoint* aStartPoint,
    EditorDOMPoint* aEndPoint) {
  MOZ_ASSERT(aStartPoint);
  MOZ_ASSERT(aEndPoint);

  if (NS_WARN_IF(!aStartPoint->IsSet()) || NS_WARN_IF(!aEndPoint->IsSet())) {
    return NS_ERROR_INVALID_ARG;
  }

  AutoTrackDOMPoint trackerStart(aHTMLEditor.RangeUpdaterRef(), aStartPoint);
  AutoTrackDOMPoint trackerEnd(aHTMLEditor.RangeUpdaterRef(), aEndPoint);

  nsresult rv = WhiteSpaceVisibilityKeeper::
      MakeSureToKeepVisibleStateOfWhiteSpacesAroundDeletingRange(
          aHTMLEditor, EditorDOMRange(*aStartPoint, *aEndPoint));
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "WhiteSpaceVisibilityKeeper::"
      "MakeSureToKeepVisibleStateOfWhiteSpacesAroundDeletingRange() failed");
  return rv;
}

nsresult WhiteSpaceVisibilityKeeper::PrepareToDeleteNode(
    HTMLEditor& aHTMLEditor, nsIContent* aContent) {
  if (NS_WARN_IF(!aContent)) {
    return NS_ERROR_INVALID_ARG;
  }

  EditorRawDOMPoint atContent(aContent);
  if (!atContent.IsSet()) {
    NS_WARNING("aContent was an orphan node");
    return NS_ERROR_INVALID_ARG;
  }

  nsresult rv = WhiteSpaceVisibilityKeeper::
      MakeSureToKeepVisibleStateOfWhiteSpacesAroundDeletingRange(
          aHTMLEditor, EditorDOMRange(atContent, atContent.NextPoint()));
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "WhiteSpaceVisibilityKeeper::"
      "MakeSureToKeepVisibleStateOfWhiteSpacesAroundDeletingRange() failed");
  return rv;
}

nsresult WhiteSpaceVisibilityKeeper::PrepareToSplitAcrossBlocks(
    HTMLEditor& aHTMLEditor, nsCOMPtr<nsINode>* aSplitNode,
    int32_t* aSplitOffset) {
  if (NS_WARN_IF(!aSplitNode) || NS_WARN_IF(!*aSplitNode) ||
      NS_WARN_IF(!aSplitOffset)) {
    return NS_ERROR_INVALID_ARG;
  }

  AutoTrackDOMPoint tracker(aHTMLEditor.RangeUpdaterRef(), aSplitNode,
                            aSplitOffset);

  nsresult rv = WhiteSpaceVisibilityKeeper::
      MakeSureToKeepVisibleWhiteSpacesVisibleAfterSplit(
          aHTMLEditor, EditorDOMPoint(*aSplitNode, *aSplitOffset));
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "WhiteSpaceVisibilityKeeper::"
                       "MakeSureToKeepVisibleWhiteSpacesVisibleAfterSplit() "
                       "failed");
  return rv;
}

// static
Result<RefPtr<Element>, nsresult> WhiteSpaceVisibilityKeeper::InsertBRElement(
    HTMLEditor& aHTMLEditor, const EditorDOMPoint& aPointToInsert) {
  if (NS_WARN_IF(!aPointToInsert.IsSet())) {
    return Err(NS_ERROR_INVALID_ARG);
  }

  // MOOSE: for now, we always assume non-PRE formatting.  Fix this later.
  // meanwhile, the pre case is handled in HandleInsertText() in
  // HTMLEditSubActionHandler.cpp

  Element* editingHost = aHTMLEditor.GetActiveEditingHost();
  TextFragmentData textFragmentDataAtInsertionPoint(aPointToInsert,
                                                    editingHost);
  const EditorDOMRange invisibleLeadingWhiteSpaceRangeOfNewLine =
      textFragmentDataAtInsertionPoint
          .GetNewInvisibleLeadingWhiteSpaceRangeIfSplittingAt(aPointToInsert);
  const EditorDOMRange invisibleTrailingWhiteSpaceRangeOfCurrentLine =
      textFragmentDataAtInsertionPoint
          .GetNewInvisibleTrailingWhiteSpaceRangeIfSplittingAt(aPointToInsert);
  const Maybe<const VisibleWhiteSpacesData> visibleWhiteSpaces =
      !invisibleLeadingWhiteSpaceRangeOfNewLine.IsPositioned() ||
              !invisibleTrailingWhiteSpaceRangeOfCurrentLine.IsPositioned()
          ? Some(textFragmentDataAtInsertionPoint.VisibleWhiteSpacesDataRef())
          : Nothing();
  const PointPosition pointPositionWithVisibleWhiteSpaces =
      visibleWhiteSpaces.isSome() && visibleWhiteSpaces.ref().IsInitialized()
          ? visibleWhiteSpaces.ref().ComparePoint(aPointToInsert)
          : PointPosition::NotInSameDOMTree;

  EditorDOMPoint pointToInsert(aPointToInsert);
  {
    // Some scoping for AutoTrackDOMPoint.  This will track our insertion
    // point while we tweak any surrounding white-space
    AutoTrackDOMPoint tracker(aHTMLEditor.RangeUpdaterRef(), &pointToInsert);

    if (invisibleTrailingWhiteSpaceRangeOfCurrentLine.IsPositioned()) {
      if (!invisibleTrailingWhiteSpaceRangeOfCurrentLine.Collapsed()) {
        // XXX Why don't we remove all of the invisible white-spaces?
        MOZ_ASSERT(invisibleTrailingWhiteSpaceRangeOfCurrentLine.StartRef() ==
                   pointToInsert);
        nsresult rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
            invisibleTrailingWhiteSpaceRangeOfCurrentLine.StartRef(),
            invisibleTrailingWhiteSpaceRangeOfCurrentLine.EndRef());
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
          return Err(rv);
        }
      }
    }
    // If new line will start with visible white-spaces, it needs to be start
    // with an NBSP.
    else if (pointPositionWithVisibleWhiteSpaces ==
                 PointPosition::StartOfFragment ||
             pointPositionWithVisibleWhiteSpaces ==
                 PointPosition::MiddleOfFragment) {
      EditorRawDOMPointInText atNextCharOfInsertionPoint =
          textFragmentDataAtInsertionPoint.GetInclusiveNextEditableCharPoint(
              pointToInsert);
      if (atNextCharOfInsertionPoint.IsSet() &&
          !atNextCharOfInsertionPoint.IsEndOfContainer() &&
          atNextCharOfInsertionPoint.IsCharASCIISpace() &&
          !EditorUtils::IsContentPreformatted(
              *atNextCharOfInsertionPoint.ContainerAsText())) {
        EditorRawDOMPointInText atPreviousCharOfNextCharOfInsertionPoint =
            textFragmentDataAtInsertionPoint.GetPreviousEditableCharPoint(
                atNextCharOfInsertionPoint);
        if (!atPreviousCharOfNextCharOfInsertionPoint.IsSet() ||
            atPreviousCharOfNextCharOfInsertionPoint.IsEndOfContainer() ||
            !atPreviousCharOfNextCharOfInsertionPoint.IsCharASCIISpace()) {
          // We are at start of non-nbsps.  Convert to a single nbsp.
          EditorRawDOMPointInText endOfCollapsibleASCIIWhiteSpaces =
              textFragmentDataAtInsertionPoint
                  .GetEndOfCollapsibleASCIIWhiteSpaces(
                      atNextCharOfInsertionPoint);
          nsresult rv =
              WhiteSpaceVisibilityKeeper::ReplaceTextAndRemoveEmptyTextNodes(
                  aHTMLEditor,
                  EditorDOMRangeInTexts(atNextCharOfInsertionPoint,
                                        endOfCollapsibleASCIIWhiteSpaces),
                  nsDependentSubstring(&kNBSP, 1));
          if (NS_FAILED(rv)) {
            NS_WARNING(
                "WhiteSpaceVisibilityKeeper::"
                "ReplaceTextAndRemoveEmptyTextNodes() failed");
            return Err(rv);
          }
        }
      }
    }

    if (invisibleLeadingWhiteSpaceRangeOfNewLine.IsPositioned()) {
      if (!invisibleLeadingWhiteSpaceRangeOfNewLine.Collapsed()) {
        // XXX Why don't we remove all of the invisible white-spaces?
        MOZ_ASSERT(invisibleLeadingWhiteSpaceRangeOfNewLine.EndRef() ==
                   pointToInsert);
        // XXX If the DOM tree has been changed above,
        //     invisibleLeadingWhiteSpaceRangeOfNewLine may be invalid now.
        //     So, we may do something wrong here.
        nsresult rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
            invisibleLeadingWhiteSpaceRangeOfNewLine.StartRef(),
            invisibleLeadingWhiteSpaceRangeOfNewLine.EndRef());
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "WhiteSpaceVisibilityKeeper::"
              "DeleteTextAndTextNodesWithTransaction() failed");
          return Err(rv);
        }
      }
    }
    // If the `<br>` element is put immediately after an NBSP, it should be
    // replaced with an ASCII white-space.
    else if (pointPositionWithVisibleWhiteSpaces ==
                 PointPosition::MiddleOfFragment ||
             pointPositionWithVisibleWhiteSpaces ==
                 PointPosition::EndOfFragment) {
      // XXX If the DOM tree has been changed above, pointToInsert` and/or
      //     `visibleWhiteSpaces` may be invalid.  So, we may do
      //     something wrong here.
      EditorDOMPointInText atNBSPReplacedWithASCIIWhiteSpace =
          textFragmentDataAtInsertionPoint
              .GetPreviousNBSPPointIfNeedToReplaceWithASCIIWhiteSpace(
                  pointToInsert);
      if (atNBSPReplacedWithASCIIWhiteSpace.IsSet()) {
        AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
        nsresult rv = aHTMLEditor.ReplaceTextWithTransaction(
            MOZ_KnownLive(*atNBSPReplacedWithASCIIWhiteSpace.ContainerAsText()),
            atNBSPReplacedWithASCIIWhiteSpace.Offset(), 1, u" "_ns);
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::ReplaceTextWithTransaction() failed failed");
          return Err(rv);
        }
      }
    }
  }

  RefPtr<Element> newBRElement = aHTMLEditor.InsertBRElementWithTransaction(
      pointToInsert, nsIEditor::eNone);
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    return Err(NS_ERROR_EDITOR_DESTROYED);
  }
  if (!newBRElement) {
    NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
    return Err(NS_ERROR_FAILURE);
  }
  return newBRElement;
}

// static
nsresult WhiteSpaceVisibilityKeeper::ReplaceText(
    HTMLEditor& aHTMLEditor, const nsAString& aStringToInsert,
    const EditorDOMRange& aRangeToBeReplaced,
    EditorRawDOMPoint* aPointAfterInsertedString /* = nullptr */) {
  // MOOSE: for now, we always assume non-PRE formatting.  Fix this later.
  // meanwhile, the pre case is handled in HandleInsertText() in
  // HTMLEditSubActionHandler.cpp

  // MOOSE: for now, just getting the ws logic straight.  This implementation
  // is very slow.  Will need to replace edit rules impl with a more efficient
  // text sink here that does the minimal amount of searching/replacing/copying

  if (aStringToInsert.IsEmpty()) {
    MOZ_ASSERT(aRangeToBeReplaced.Collapsed());
    if (aPointAfterInsertedString) {
      *aPointAfterInsertedString = aRangeToBeReplaced.StartRef();
    }
    return NS_OK;
  }

  RefPtr<Element> editingHost = aHTMLEditor.GetActiveEditingHost();
  TextFragmentData textFragmentDataAtStart(aRangeToBeReplaced.StartRef(),
                                           editingHost);
  const bool insertionPointEqualsOrIsBeforeStartOfText =
      aRangeToBeReplaced.StartRef().EqualsOrIsBefore(
          textFragmentDataAtStart.StartRef());
  TextFragmentData textFragmentDataAtEnd =
      aRangeToBeReplaced.Collapsed()
          ? textFragmentDataAtStart
          : TextFragmentData(aRangeToBeReplaced.EndRef(), editingHost);
  const bool insertionPointAfterEndOfText =
      textFragmentDataAtEnd.EndRef().EqualsOrIsBefore(
          aRangeToBeReplaced.EndRef());

  const EditorDOMRange invisibleLeadingWhiteSpaceRangeAtStart =
      textFragmentDataAtStart
          .GetNewInvisibleLeadingWhiteSpaceRangeIfSplittingAt(
              aRangeToBeReplaced.StartRef());
  const EditorDOMRange invisibleTrailingWhiteSpaceRangeAtEnd =
      textFragmentDataAtEnd.GetNewInvisibleTrailingWhiteSpaceRangeIfSplittingAt(
          aRangeToBeReplaced.EndRef());
  const Maybe<const VisibleWhiteSpacesData> visibleWhiteSpacesAtStart =
      !invisibleLeadingWhiteSpaceRangeAtStart.IsPositioned()
          ? Some(textFragmentDataAtStart.VisibleWhiteSpacesDataRef())
          : Nothing();
  const PointPosition pointPositionWithVisibleWhiteSpacesAtStart =
      visibleWhiteSpacesAtStart.isSome() &&
              visibleWhiteSpacesAtStart.ref().IsInitialized()
          ? visibleWhiteSpacesAtStart.ref().ComparePoint(
                aRangeToBeReplaced.StartRef())
          : PointPosition::NotInSameDOMTree;
  const Maybe<const VisibleWhiteSpacesData> visibleWhiteSpacesAtEnd =
      !invisibleTrailingWhiteSpaceRangeAtEnd.IsPositioned()
          ? Some(textFragmentDataAtEnd.VisibleWhiteSpacesDataRef())
          : Nothing();
  const PointPosition pointPositionWithVisibleWhiteSpacesAtEnd =
      visibleWhiteSpacesAtEnd.isSome() &&
              visibleWhiteSpacesAtEnd.ref().IsInitialized()
          ? visibleWhiteSpacesAtEnd.ref().ComparePoint(
                aRangeToBeReplaced.EndRef())
          : PointPosition::NotInSameDOMTree;

  EditorDOMPoint pointToInsert(aRangeToBeReplaced.StartRef());
  nsAutoString theString(aStringToInsert);
  {
    // Some scoping for AutoTrackDOMPoint.  This will track our insertion
    // point while we tweak any surrounding white-space
    AutoTrackDOMPoint tracker(aHTMLEditor.RangeUpdaterRef(), &pointToInsert);

    if (invisibleTrailingWhiteSpaceRangeAtEnd.IsPositioned()) {
      if (!invisibleTrailingWhiteSpaceRangeAtEnd.Collapsed()) {
        // XXX Why don't we remove all of the invisible white-spaces?
        MOZ_ASSERT(invisibleTrailingWhiteSpaceRangeAtEnd.StartRef() ==
                   pointToInsert);
        nsresult rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
            invisibleTrailingWhiteSpaceRangeAtEnd.StartRef(),
            invisibleTrailingWhiteSpaceRangeAtEnd.EndRef());
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
          return rv;
        }
      }
    }
    // Replace an NBSP at inclusive next character of replacing range to an
    // ASCII white-space if inserting into a visible white-space sequence.
    // XXX With modifying the inserting string later, this creates a line break
    //     opportunity after the inserting string, but this causes
    //     inconsistent result with inserting order.  E.g., type white-space
    //     n times with various order.
    else if (pointPositionWithVisibleWhiteSpacesAtEnd ==
                 PointPosition::StartOfFragment ||
             pointPositionWithVisibleWhiteSpacesAtEnd ==
                 PointPosition::MiddleOfFragment) {
      EditorDOMPointInText atNBSPReplacedWithASCIIWhiteSpace =
          textFragmentDataAtEnd
              .GetInclusiveNextNBSPPointIfNeedToReplaceWithASCIIWhiteSpace(
                  pointToInsert);
      if (atNBSPReplacedWithASCIIWhiteSpace.IsSet()) {
        AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
        nsresult rv = aHTMLEditor.ReplaceTextWithTransaction(
            MOZ_KnownLive(*atNBSPReplacedWithASCIIWhiteSpace.ContainerAsText()),
            atNBSPReplacedWithASCIIWhiteSpace.Offset(), 1, u" "_ns);
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::ReplaceTextWithTransaction() failed");
          return rv;
        }
      }
    }

    if (invisibleLeadingWhiteSpaceRangeAtStart.IsPositioned()) {
      if (!invisibleLeadingWhiteSpaceRangeAtStart.Collapsed()) {
        // XXX Why don't we remove all of the invisible white-spaces?
        MOZ_ASSERT(invisibleLeadingWhiteSpaceRangeAtStart.EndRef() ==
                   pointToInsert);
        // XXX If the DOM tree has been changed above,
        //     invisibleLeadingWhiteSpaceRangeAtStart may be invalid now.
        //     So, we may do something wrong here.
        nsresult rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
            invisibleLeadingWhiteSpaceRangeAtStart.StartRef(),
            invisibleLeadingWhiteSpaceRangeAtStart.EndRef());
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
          return rv;
        }
      }
    }
    // Replace an NBSP at previous character of insertion point to an ASCII
    // white-space if inserting into a visible white-space sequence.
    // XXX With modifying the inserting string later, this creates a line break
    //     opportunity before the inserting string, but this causes
    //     inconsistent result with inserting order.  E.g., type white-space
    //     n times with various order.
    else if (pointPositionWithVisibleWhiteSpacesAtStart ==
                 PointPosition::MiddleOfFragment ||
             pointPositionWithVisibleWhiteSpacesAtStart ==
                 PointPosition::EndOfFragment) {
      // XXX If the DOM tree has been changed above, pointToInsert` and/or
      //     `visibleWhiteSpaces` may be invalid.  So, we may do
      //     something wrong here.
      EditorDOMPointInText atNBSPReplacedWithASCIIWhiteSpace =
          textFragmentDataAtStart
              .GetPreviousNBSPPointIfNeedToReplaceWithASCIIWhiteSpace(
                  pointToInsert);
      if (atNBSPReplacedWithASCIIWhiteSpace.IsSet()) {
        AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
        nsresult rv = aHTMLEditor.ReplaceTextWithTransaction(
            MOZ_KnownLive(*atNBSPReplacedWithASCIIWhiteSpace.ContainerAsText()),
            atNBSPReplacedWithASCIIWhiteSpace.Offset(), 1, u" "_ns);
        if (NS_FAILED(rv)) {
          NS_WARNING("HTMLEditor::ReplaceTextWithTransaction() failed failed");
          return rv;
        }
      }
    }

    // After this block, pointToInsert is modified by AutoTrackDOMPoint.
  }

  // Next up, tweak head and tail of string as needed.  First the head: there
  // are a variety of circumstances that would require us to convert a leading
  // ws char into an nbsp:

  if (nsCRT::IsAsciiSpace(theString[0])) {
    // If inserting string will follow some invisible leading white-spaces, the
    // string needs to start with an NBSP.
    if (invisibleLeadingWhiteSpaceRangeAtStart.IsPositioned()) {
      theString.SetCharAt(kNBSP, 0);
    }
    // If inserting around visible white-spaces, check whether the previous
    // character of insertion point is an NBSP or an ASCII white-space.
    else if (pointPositionWithVisibleWhiteSpacesAtStart ==
                 PointPosition::MiddleOfFragment ||
             pointPositionWithVisibleWhiteSpacesAtStart ==
                 PointPosition::EndOfFragment) {
      EditorDOMPointInText atPreviousChar =
          textFragmentDataAtStart.GetPreviousEditableCharPoint(pointToInsert);
      if (atPreviousChar.IsSet() && !atPreviousChar.IsEndOfContainer() &&
          atPreviousChar.IsCharASCIISpace()) {
        theString.SetCharAt(kNBSP, 0);
      }
    }
    // If the insertion point is (was) before the start of text and it's
    // immediately after a hard line break, the first ASCII white-space should
    // be replaced with an NBSP for making it visible.
    else if (textFragmentDataAtStart.StartsFromHardLineBreak() &&
             insertionPointEqualsOrIsBeforeStartOfText) {
      theString.SetCharAt(kNBSP, 0);
    }
  }

  // Then the tail
  uint32_t lastCharIndex = theString.Length() - 1;

  if (nsCRT::IsAsciiSpace(theString[lastCharIndex])) {
    // If inserting string will be followed by some invisible trailing
    // white-spaces, the string needs to end with an NBSP.
    if (invisibleTrailingWhiteSpaceRangeAtEnd.IsPositioned()) {
      theString.SetCharAt(kNBSP, lastCharIndex);
    }
    // If inserting around visible white-spaces, check whether the inclusive
    // next character of end of replaced range is an NBSP or an ASCII
    // white-space.
    if (pointPositionWithVisibleWhiteSpacesAtStart ==
            PointPosition::StartOfFragment ||
        pointPositionWithVisibleWhiteSpacesAtStart ==
            PointPosition::MiddleOfFragment) {
      EditorDOMPointInText atNextChar =
          textFragmentDataAtStart.GetInclusiveNextEditableCharPoint(
              pointToInsert);
      if (atNextChar.IsSet() && !atNextChar.IsEndOfContainer() &&
          atNextChar.IsCharASCIISpace()) {
        theString.SetCharAt(kNBSP, lastCharIndex);
      }
    }
    // If the end of replacing range is (was) after the end of text and it's
    // immediately before block boundary, the last ASCII white-space should
    // be replaced with an NBSP for making it visible.
    else if (textFragmentDataAtEnd.EndsByBlockBoundary() &&
             insertionPointAfterEndOfText) {
      theString.SetCharAt(kNBSP, lastCharIndex);
    }
  }

  // Next, scan string for adjacent ws and convert to nbsp/space combos
  // MOOSE: don't need to convert tabs here since that is done by
  // WillInsertText() before we are called.  Eventually, all that logic will be
  // pushed down into here and made more efficient.
  bool prevWS = false;
  for (uint32_t i = 0; i <= lastCharIndex; i++) {
    if (nsCRT::IsAsciiSpace(theString[i])) {
      if (prevWS) {
        // i - 1 can't be negative because prevWS starts out false
        theString.SetCharAt(kNBSP, i - 1);
      } else {
        prevWS = true;
      }
    } else {
      prevWS = false;
    }
  }

  // XXX If the point is not editable, InsertTextWithTransaction() returns
  //     error, but we keep handling it.  But I think that it wastes the
  //     runtime cost.  So, perhaps, we should return error code which couldn't
  //     modify it and make each caller of this method decide whether it should
  //     keep or stop handling the edit action.
  if (!aHTMLEditor.GetDocument()) {
    NS_WARNING(
        "WhiteSpaceVisibilityKeeper::ReplaceText() lost proper document");
    return NS_ERROR_UNEXPECTED;
  }
  OwningNonNull<Document> document = *aHTMLEditor.GetDocument();
  nsresult rv = aHTMLEditor.InsertTextWithTransaction(
      document, theString, pointToInsert, aPointAfterInsertedString);
  if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
    return NS_ERROR_EDITOR_DESTROYED;
  }
  if (NS_SUCCEEDED(rv)) {
    return NS_OK;
  }

  NS_WARNING("HTMLEditor::InsertTextWithTransaction() failed, but ignored");

  // XXX Temporarily, set new insertion point to the original point.
  if (aPointAfterInsertedString) {
    *aPointAfterInsertedString = pointToInsert;
  }
  return NS_OK;
}

// static
nsresult WhiteSpaceVisibilityKeeper::DeletePreviousWhiteSpace(
    HTMLEditor& aHTMLEditor, const EditorDOMPoint& aPoint) {
  Element* editingHost = aHTMLEditor.GetActiveEditingHost();
  TextFragmentData textFragmentDataAtDeletion(aPoint, editingHost);
  EditorDOMPointInText atPreviousCharOfStart =
      textFragmentDataAtDeletion.GetPreviousEditableCharPoint(aPoint);
  if (!atPreviousCharOfStart.IsSet() ||
      atPreviousCharOfStart.IsEndOfContainer()) {
    return NS_OK;
  }

  // Easy case, preformatted ws.
  if (EditorUtils::IsContentPreformatted(
          *atPreviousCharOfStart.ContainerAsText())) {
    if (!atPreviousCharOfStart.IsCharASCIISpace() &&
        !atPreviousCharOfStart.IsCharNBSP()) {
      return NS_OK;
    }
    nsresult rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
        atPreviousCharOfStart, atPreviousCharOfStart.NextPoint());
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
    return rv;
  }

  // Caller's job to ensure that previous char is really ws.  If it is normal
  // ws, we need to delete the whole run.
  if (atPreviousCharOfStart.IsCharASCIISpace()) {
    EditorDOMPoint startToDelete =
        textFragmentDataAtDeletion.GetFirstASCIIWhiteSpacePointCollapsedTo(
            atPreviousCharOfStart);
    EditorDOMPoint endToDelete =
        textFragmentDataAtDeletion.GetEndOfCollapsibleASCIIWhiteSpaces(
            atPreviousCharOfStart);
    nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToDeleteRange(
        aHTMLEditor, &startToDelete, &endToDelete);
    if (NS_FAILED(rv)) {
      NS_WARNING("WhiteSpaceVisibilityKeeper::PrepareToDeleteRange() failed");
      return rv;
    }

    // finally, delete that ws
    rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(startToDelete,
                                                           endToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
    return rv;
  }

  if (atPreviousCharOfStart.IsCharNBSP()) {
    EditorDOMPoint startToDelete(atPreviousCharOfStart);
    EditorDOMPoint endToDelete(startToDelete.NextPoint());
    nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToDeleteRange(
        aHTMLEditor, &startToDelete, &endToDelete);
    if (NS_FAILED(rv)) {
      NS_WARNING("WhiteSpaceVisibilityKeeper::PrepareToDeleteRange() failed");
      return rv;
    }

    // finally, delete that ws
    rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(startToDelete,
                                                           endToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
    return rv;
  }

  return NS_OK;
}

// static
nsresult WhiteSpaceVisibilityKeeper::DeleteInclusiveNextWhiteSpace(
    HTMLEditor& aHTMLEditor, const EditorDOMPoint& aPoint) {
  Element* editingHost = aHTMLEditor.GetActiveEditingHost();
  TextFragmentData textFragmentDataAtDeletion(aPoint, editingHost);
  EditorDOMPointInText atNextCharOfStart =
      textFragmentDataAtDeletion.GetInclusiveNextEditableCharPoint(aPoint);
  if (!atNextCharOfStart.IsSet() || atNextCharOfStart.IsEndOfContainer()) {
    return NS_OK;
  }

  // Easy case, preformatted ws.
  if (EditorUtils::IsContentPreformatted(
          *atNextCharOfStart.ContainerAsText())) {
    if (!atNextCharOfStart.IsCharASCIISpace() &&
        !atNextCharOfStart.IsCharNBSP()) {
      return NS_OK;
    }
    nsresult rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
        atNextCharOfStart, atNextCharOfStart.NextPoint());
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
    return rv;
  }

  // Caller's job to ensure that next char is really ws.  If it is normal ws,
  // we need to delete the whole run.
  if (atNextCharOfStart.IsCharASCIISpace()) {
    EditorDOMPoint startToDelete =
        textFragmentDataAtDeletion.GetFirstASCIIWhiteSpacePointCollapsedTo(
            atNextCharOfStart);
    EditorDOMPoint endToDelete =
        textFragmentDataAtDeletion.GetEndOfCollapsibleASCIIWhiteSpaces(
            atNextCharOfStart);
    nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToDeleteRange(
        aHTMLEditor, &startToDelete, &endToDelete);
    if (NS_FAILED(rv)) {
      NS_WARNING("WhiteSpaceVisibilityKeeper::PrepareToDeleteRange() failed");
      return rv;
    }

    // Finally, delete that ws
    rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(startToDelete,
                                                           endToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
    return rv;
  }

  if (atNextCharOfStart.IsCharNBSP()) {
    EditorDOMPoint startToDelete(atNextCharOfStart);
    EditorDOMPoint endToDelete(startToDelete.NextPoint());
    nsresult rv = WhiteSpaceVisibilityKeeper::PrepareToDeleteRange(
        aHTMLEditor, &startToDelete, &endToDelete);
    if (NS_FAILED(rv)) {
      NS_WARNING("WhiteSpaceVisibilityKeeper::PrepareToDeleteRange() failed");
      return rv;
    }

    // Finally, delete that ws
    rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(startToDelete,
                                                           endToDelete);
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
    return rv;
  }

  return NS_OK;
}

template <typename PT, typename CT>
WSScanResult WSRunScanner::ScanPreviousVisibleNodeOrBlockBoundaryFrom(
    const EditorDOMPointBase<PT, CT>& aPoint) const {
  MOZ_ASSERT(aPoint.IsSet());

  // If the range has visible text and start of the visible text is before
  // aPoint, return previous character in the text.
  const VisibleWhiteSpacesData& visibleWhiteSpaces =
      TextFragmentDataAtStartRef().VisibleWhiteSpacesDataRef();
  if (visibleWhiteSpaces.IsInitialized() &&
      visibleWhiteSpaces.StartRef().IsBefore(aPoint)) {
    EditorDOMPointInText atPreviousChar = GetPreviousEditableCharPoint(aPoint);
    // When it's a non-empty text node, return it.
    if (atPreviousChar.IsSet() && !atPreviousChar.IsContainerEmpty()) {
      MOZ_ASSERT(!atPreviousChar.IsEndOfContainer());
      return WSScanResult(atPreviousChar.NextPoint(),
                          atPreviousChar.IsCharASCIISpaceOrNBSP()
                              ? WSType::NormalWhiteSpaces
                              : WSType::NormalText);
    }
  }

  // Otherwise, return the start of the range.
  if (TextFragmentDataAtStartRef().GetStartReasonContent() !=
      TextFragmentDataAtStartRef().StartRef().GetContainer()) {
    // In this case, TextFragmentDataAtStartRef().StartRef().Offset() is not
    // meaningful.
    return WSScanResult(TextFragmentDataAtStartRef().GetStartReasonContent(),
                        TextFragmentDataAtStartRef().StartRawReason());
  }
  return WSScanResult(TextFragmentDataAtStartRef().StartRef(),
                      TextFragmentDataAtStartRef().StartRawReason());
}

template <typename PT, typename CT>
WSScanResult WSRunScanner::ScanNextVisibleNodeOrBlockBoundaryFrom(
    const EditorDOMPointBase<PT, CT>& aPoint) const {
  MOZ_ASSERT(aPoint.IsSet());

  // If the range has visible text and aPoint equals or is before the end of the
  // visible text, return inclusive next character in the text.
  const VisibleWhiteSpacesData& visibleWhiteSpaces =
      TextFragmentDataAtStartRef().VisibleWhiteSpacesDataRef();
  if (visibleWhiteSpaces.IsInitialized() &&
      aPoint.EqualsOrIsBefore(visibleWhiteSpaces.EndRef())) {
    EditorDOMPointInText atNextChar = GetInclusiveNextEditableCharPoint(aPoint);
    // When it's a non-empty text node, return it.
    if (atNextChar.IsSet() && !atNextChar.IsContainerEmpty()) {
      return WSScanResult(
          atNextChar,
          !atNextChar.IsEndOfContainer() && atNextChar.IsCharASCIISpaceOrNBSP()
              ? WSType::NormalWhiteSpaces
              : WSType::NormalText);
    }
  }

  // Otherwise, return the end of the range.
  if (TextFragmentDataAtStartRef().GetEndReasonContent() !=
      TextFragmentDataAtStartRef().EndRef().GetContainer()) {
    // In this case, TextFragmentDataAtStartRef().EndRef().Offset() is not
    // meaningful.
    return WSScanResult(TextFragmentDataAtStartRef().GetEndReasonContent(),
                        TextFragmentDataAtStartRef().EndRawReason());
  }
  return WSScanResult(TextFragmentDataAtStartRef().EndRef(),
                      TextFragmentDataAtStartRef().EndRawReason());
}

template <typename EditorDOMPointType>
WSRunScanner::TextFragmentData::TextFragmentData(
    const EditorDOMPointType& aPoint, const Element* aEditingHost)
    : mEditingHost(aEditingHost), mIsPreformatted(false) {
  if (!aPoint.IsSetAndValid()) {
    NS_WARNING("aPoint was invalid");
    return;
  }
  if (!aPoint.IsInContentNode()) {
    NS_WARNING("aPoint was in Document or DocumentFragment");
    // I.e., we're try to modify outside of root element.  We don't need to
    // support such odd case because web apps cannot append text nodes as
    // direct child of Document node.
    return;
  }

  mScanStartPoint = aPoint;
  NS_ASSERTION(EditorUtils::IsEditableContent(
                   *mScanStartPoint.ContainerAsContent(), EditorType::HTML),
               "Given content is not editable");
  NS_ASSERTION(
      mScanStartPoint.ContainerAsContent()->GetAsElementOrParentElement(),
      "Given content is not an element and an orphan node");
  nsIContent* editableBlockParentOrTopmotEditableInlineContent =
      EditorUtils::IsEditableContent(*mScanStartPoint.ContainerAsContent(),
                                     EditorType::HTML)
          ? HTMLEditUtils::
                GetInclusiveAncestorEditableBlockElementOrInlineEditingHost(
                    *mScanStartPoint.ContainerAsContent())
          : nullptr;
  if (!editableBlockParentOrTopmotEditableInlineContent) {
    // Meaning that the container of `mScanStartPoint` is not editable.
    editableBlockParentOrTopmotEditableInlineContent =
        mScanStartPoint.ContainerAsContent();
  }

  mStart = BoundaryData::ScanCollapsibleWhiteSpaceStartFrom(
      mScanStartPoint, *editableBlockParentOrTopmotEditableInlineContent,
      mEditingHost, &mNBSPData);
  mEnd = BoundaryData::ScanCollapsibleWhiteSpaceEndFrom(
      mScanStartPoint, *editableBlockParentOrTopmotEditableInlineContent,
      mEditingHost, &mNBSPData);
  // If scan start point is start/end of preformatted text node, only
  // mEnd/mStart crosses a preformatted character so that when one of
  // them crosses a preformatted character, this fragment's range is
  // preformatted.
  // Additionally, if the scan start point is preformatted, and there is
  // no text node around it, the range is also preformatted.
  mIsPreformatted = mStart.AcrossPreformattedCharacter() ||
                    mEnd.AcrossPreformattedCharacter() ||
                    (EditorUtils::IsContentPreformatted(
                         *mScanStartPoint.ContainerAsContent()) &&
                     !mStart.IsNormalText() && !mEnd.IsNormalText());
}

// static
template <typename EditorDOMPointType>
Maybe<WSRunScanner::TextFragmentData::BoundaryData> WSRunScanner::
    TextFragmentData::BoundaryData::ScanCollapsibleWhiteSpaceStartInTextNode(
        const EditorDOMPointType& aPoint, NoBreakingSpaceData* aNBSPData) {
  MOZ_ASSERT(aPoint.IsSetAndValid());
  MOZ_DIAGNOSTIC_ASSERT(aPoint.IsInTextNode());
  MOZ_DIAGNOSTIC_ASSERT(
      !EditorUtils::IsContentPreformatted(*aPoint.ContainerAsText()));

  const nsTextFragment& textFragment = aPoint.ContainerAsText()->TextFragment();
  for (uint32_t i = std::min(aPoint.Offset(), textFragment.GetLength()); i;
       i--) {
    char16_t ch = textFragment.CharAt(i - 1);
    if (nsCRT::IsAsciiSpace(ch)) {
      continue;
    }

    if (ch == HTMLEditUtils::kNBSP) {
      if (aNBSPData) {
        aNBSPData->NotifyNBSP(
            EditorDOMPointInText(aPoint.ContainerAsText(), i - 1),
            NoBreakingSpaceData::Scanning::Backward);
      }
      continue;
    }

    return Some(BoundaryData(EditorDOMPoint(aPoint.ContainerAsText(), i),
                             *aPoint.ContainerAsText(), WSType::NormalText,
                             Preformatted::No));
  }

  return Nothing();
}

// static
template <typename EditorDOMPointType>
WSRunScanner::TextFragmentData::BoundaryData WSRunScanner::TextFragmentData::
    BoundaryData::ScanCollapsibleWhiteSpaceStartFrom(
        const EditorDOMPointType& aPoint,
        const nsIContent& aEditableBlockParentOrTopmostEditableInlineContent,
        const Element* aEditingHost, NoBreakingSpaceData* aNBSPData) {
  MOZ_ASSERT(aPoint.IsSetAndValid());

  if (aPoint.IsInTextNode() && !aPoint.IsStartOfContainer()) {
    // If the point is in a text node which is preformatted, we should return
    // the point as a visible character point.
    if (EditorUtils::IsContentPreformatted(*aPoint.ContainerAsText())) {
      return BoundaryData(aPoint, *aPoint.ContainerAsText(), WSType::NormalText,
                          Preformatted::Yes);
    }
    // If the text node is not preformatted, we should look for its preceding
    // characters.
    Maybe<BoundaryData> startInTextNode =
        BoundaryData::ScanCollapsibleWhiteSpaceStartInTextNode(aPoint,
                                                               aNBSPData);
    if (startInTextNode.isSome()) {
      return startInTextNode.ref();
    }
    // The text node does not have visible character, let's keep scanning
    // preceding nodes.
    return BoundaryData::ScanCollapsibleWhiteSpaceStartFrom(
        EditorDOMPoint(aPoint.ContainerAsText(), 0),
        aEditableBlockParentOrTopmostEditableInlineContent, aEditingHost,
        aNBSPData);
  }

  // Then, we need to check previous leaf node.
  nsIContent* previousLeafContentOrBlock =
      HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
          aPoint, aEditableBlockParentOrTopmostEditableInlineContent,
          aEditingHost);
  if (!previousLeafContentOrBlock) {
    // no prior node means we exhausted
    // aEditableBlockParentOrTopmostEditableInlineContent
    // mReasonContent can be either a block element or any non-editable
    // content in this case.
    return BoundaryData(aPoint,
                        const_cast<nsIContent&>(
                            aEditableBlockParentOrTopmostEditableInlineContent),
                        WSType::CurrentBlockBoundary, Preformatted::No);
  }

  if (HTMLEditUtils::IsBlockElement(*previousLeafContentOrBlock)) {
    return BoundaryData(aPoint, *previousLeafContentOrBlock,
                        WSType::OtherBlockBoundary, Preformatted::No);
  }

  if (!previousLeafContentOrBlock->IsText() ||
      !previousLeafContentOrBlock->IsEditable()) {
    // it's a break or a special node, like <img>, that is not a block and
    // not a break but still serves as a terminator to ws runs.
    return BoundaryData(aPoint, *previousLeafContentOrBlock,
                        previousLeafContentOrBlock->IsHTMLElement(nsGkAtoms::br)
                            ? WSType::BRElement
                            : WSType::SpecialContent,
                        Preformatted::No);
  }

  if (!previousLeafContentOrBlock->AsText()->TextLength()) {
    // If it's an empty text node, keep looking for its previous leaf content.
    // Note that even if the empty text node is preformatted, we should keep
    // looking for the previous one.
    return BoundaryData::ScanCollapsibleWhiteSpaceStartFrom(
        EditorDOMPointInText(previousLeafContentOrBlock->AsText(), 0),
        aEditableBlockParentOrTopmostEditableInlineContent, aEditingHost,
        aNBSPData);
  }

  if (EditorUtils::IsContentPreformatted(*previousLeafContentOrBlock)) {
    // If the previous text node is preformatted and not empty, we should return
    // its end as found a visible character.  Note that we stop scanning
    // collapsible white-spaces due to reaching preformatted non-empty text
    // node.  I.e., the following text node might be not preformatted.
    return BoundaryData(EditorDOMPoint::AtEndOf(*previousLeafContentOrBlock),
                        *previousLeafContentOrBlock, WSType::NormalText,
                        Preformatted::No);
  }

  Maybe<BoundaryData> startInTextNode =
      BoundaryData::ScanCollapsibleWhiteSpaceStartInTextNode(
          EditorDOMPointInText::AtEndOf(*previousLeafContentOrBlock->AsText()),
          aNBSPData);
  if (startInTextNode.isSome()) {
    return startInTextNode.ref();
  }

  // The text node does not have visible character, let's keep scanning
  // preceding nodes.
  return BoundaryData::ScanCollapsibleWhiteSpaceStartFrom(
      EditorDOMPointInText(previousLeafContentOrBlock->AsText(), 0),
      aEditableBlockParentOrTopmostEditableInlineContent, aEditingHost,
      aNBSPData);
}

// static
template <typename EditorDOMPointType>
Maybe<WSRunScanner::TextFragmentData::BoundaryData> WSRunScanner::
    TextFragmentData::BoundaryData::ScanCollapsibleWhiteSpaceEndInTextNode(
        const EditorDOMPointType& aPoint, NoBreakingSpaceData* aNBSPData) {
  MOZ_ASSERT(aPoint.IsSetAndValid());
  MOZ_DIAGNOSTIC_ASSERT(aPoint.IsInTextNode());
  MOZ_DIAGNOSTIC_ASSERT(
      !EditorUtils::IsContentPreformatted(*aPoint.ContainerAsText()));

  const nsTextFragment& textFragment = aPoint.ContainerAsText()->TextFragment();
  for (uint32_t i = aPoint.Offset(); i < textFragment.GetLength(); i++) {
    char16_t ch = textFragment.CharAt(i);
    if (nsCRT::IsAsciiSpace(ch)) {
      continue;
    }

    if (ch == HTMLEditUtils::kNBSP) {
      if (aNBSPData) {
        aNBSPData->NotifyNBSP(EditorDOMPointInText(aPoint.ContainerAsText(), i),
                              NoBreakingSpaceData::Scanning::Forward);
      }
      continue;
    }

    return Some(BoundaryData(EditorDOMPoint(aPoint.ContainerAsText(), i),
                             *aPoint.ContainerAsText(), WSType::NormalText,
                             Preformatted::No));
  }

  return Nothing();
}

// static
template <typename EditorDOMPointType>
WSRunScanner::TextFragmentData::BoundaryData
WSRunScanner::TextFragmentData::BoundaryData::ScanCollapsibleWhiteSpaceEndFrom(
    const EditorDOMPointType& aPoint,
    const nsIContent& aEditableBlockParentOrTopmostEditableInlineContent,
    const Element* aEditingHost, NoBreakingSpaceData* aNBSPData) {
  MOZ_ASSERT(aPoint.IsSetAndValid());

  if (aPoint.IsInTextNode() && !aPoint.IsEndOfContainer()) {
    // If the point is in a text node which is preformatted, we should return
    // the point as a visible character point.
    if (EditorUtils::IsContentPreformatted(*aPoint.ContainerAsText())) {
      return BoundaryData(aPoint, *aPoint.ContainerAsText(), WSType::NormalText,
                          Preformatted::Yes);
    }
    // If the text node is not preformatted, we should look for inclusive
    // next characters.
    Maybe<BoundaryData> endInTextNode =
        BoundaryData::ScanCollapsibleWhiteSpaceEndInTextNode(aPoint, aNBSPData);
    if (endInTextNode.isSome()) {
      return endInTextNode.ref();
    }
    // The text node does not have visible character, let's keep scanning
    // following nodes.
    return BoundaryData::ScanCollapsibleWhiteSpaceEndFrom(
        EditorDOMPointInText::AtEndOf(*aPoint.ContainerAsText()),
        aEditableBlockParentOrTopmostEditableInlineContent, aEditingHost,
        aNBSPData);
  }

  // Then, we need to check next leaf node.
  nsIContent* nextLeafContentOrBlock =
      HTMLEditUtils::GetNextLeafContentOrNextBlockElement(
          aPoint, aEditableBlockParentOrTopmostEditableInlineContent,
          aEditingHost);
  if (!nextLeafContentOrBlock) {
    // no next node means we exhausted
    // aEditableBlockParentOrTopmostEditableInlineContent
    // mReasonContent can be either a block element or any non-editable
    // content in this case.
    return BoundaryData(aPoint,
                        const_cast<nsIContent&>(
                            aEditableBlockParentOrTopmostEditableInlineContent),
                        WSType::CurrentBlockBoundary, Preformatted::No);
  }

  if (HTMLEditUtils::IsBlockElement(*nextLeafContentOrBlock)) {
    // we encountered a new block.  therefore no more ws.
    return BoundaryData(aPoint, *nextLeafContentOrBlock,
                        WSType::OtherBlockBoundary, Preformatted::No);
  }

  if (!nextLeafContentOrBlock->IsText() ||
      !nextLeafContentOrBlock->IsEditable()) {
    // we encountered a break or a special node, like <img>,
    // that is not a block and not a break but still
    // serves as a terminator to ws runs.
    return BoundaryData(aPoint, *nextLeafContentOrBlock,
                        nextLeafContentOrBlock->IsHTMLElement(nsGkAtoms::br)
                            ? WSType::BRElement
                            : WSType::SpecialContent,
                        Preformatted::No);
  }

  if (!nextLeafContentOrBlock->AsText()->TextFragment().GetLength()) {
    // If it's an empty text node, keep looking for its next leaf content.
    // Note that even if the empty text node is preformatted, we should keep
    // looking for the next one.
    return BoundaryData::ScanCollapsibleWhiteSpaceEndFrom(
        EditorDOMPointInText(nextLeafContentOrBlock->AsText(), 0),
        aEditableBlockParentOrTopmostEditableInlineContent, aEditingHost,
        aNBSPData);
  }

  if (EditorUtils::IsContentPreformatted(*nextLeafContentOrBlock)) {
    // If the next text node is preformatted and not empty, we should return
    // its start as found a visible character.  Note that we stop scanning
    // collapsible white-spaces due to reaching preformatted non-empty text
    // node.  I.e., the following text node might be not preformatted.
    return BoundaryData(EditorDOMPoint(nextLeafContentOrBlock, 0),
                        *nextLeafContentOrBlock, WSType::NormalText,
                        Preformatted::No);
  }

  Maybe<BoundaryData> endInTextNode =
      BoundaryData::ScanCollapsibleWhiteSpaceEndInTextNode(
          EditorDOMPointInText(nextLeafContentOrBlock->AsText(), 0), aNBSPData);
  if (endInTextNode.isSome()) {
    return endInTextNode.ref();
  }

  // The text node does not have visible character, let's keep scanning
  // following nodes.
  return BoundaryData::ScanCollapsibleWhiteSpaceEndFrom(
      EditorDOMPointInText::AtEndOf(*nextLeafContentOrBlock->AsText()),
      aEditableBlockParentOrTopmostEditableInlineContent, aEditingHost,
      aNBSPData);
}

const EditorDOMRange&
WSRunScanner::TextFragmentData::InvisibleLeadingWhiteSpaceRangeRef() const {
  if (mLeadingWhiteSpaceRange.isSome()) {
    return mLeadingWhiteSpaceRange.ref();
  }

  // If it's preformatted or not start of line, the range is not invisible
  // leading white-spaces.
  if (!StartsFromHardLineBreak()) {
    mLeadingWhiteSpaceRange.emplace();
    return mLeadingWhiteSpaceRange.ref();
  }

  // If there is no NBSP, all of the given range is leading white-spaces.
  // Note that this result may be collapsed if there is no leading white-spaces.
  if (!mNBSPData.FoundNBSP()) {
    MOZ_ASSERT(mStart.PointRef().IsSet() || mEnd.PointRef().IsSet());
    mLeadingWhiteSpaceRange.emplace(mStart.PointRef(), mEnd.PointRef());
    return mLeadingWhiteSpaceRange.ref();
  }

  MOZ_ASSERT(mNBSPData.LastPointRef().IsSetAndValid());

  // Even if the first NBSP is the start, i.e., there is no invisible leading
  // white-space, return collapsed range.
  mLeadingWhiteSpaceRange.emplace(mStart.PointRef(), mNBSPData.FirstPointRef());
  return mLeadingWhiteSpaceRange.ref();
}

const EditorDOMRange&
WSRunScanner::TextFragmentData::InvisibleTrailingWhiteSpaceRangeRef() const {
  if (mTrailingWhiteSpaceRange.isSome()) {
    return mTrailingWhiteSpaceRange.ref();
  }

  // If it's preformatted or not immediately before block boundary, the range is
  // not invisible trailing white-spaces.  Note that collapsible white-spaces
  // before a `<br>` element is visible.
  if (!EndsByBlockBoundary()) {
    mTrailingWhiteSpaceRange.emplace();
    return mTrailingWhiteSpaceRange.ref();
  }

  // If there is no NBSP, all of the given range is trailing white-spaces.
  // Note that this result may be collapsed if there is no trailing white-
  // spaces.
  if (!mNBSPData.FoundNBSP()) {
    MOZ_ASSERT(mStart.PointRef().IsSet() || mEnd.PointRef().IsSet());
    mTrailingWhiteSpaceRange.emplace(mStart.PointRef(), mEnd.PointRef());
    return mTrailingWhiteSpaceRange.ref();
  }

  MOZ_ASSERT(mNBSPData.LastPointRef().IsSetAndValid());

  // If last NBSP is immediately before the end, there is no trailing white-
  // spaces.
  if (mEnd.PointRef().IsSet() &&
      mNBSPData.LastPointRef().GetContainer() ==
          mEnd.PointRef().GetContainer() &&
      mNBSPData.LastPointRef().Offset() == mEnd.PointRef().Offset() - 1) {
    mTrailingWhiteSpaceRange.emplace();
    return mTrailingWhiteSpaceRange.ref();
  }

  // Otherwise, the may be some trailing white-spaces.
  MOZ_ASSERT(!mNBSPData.LastPointRef().IsEndOfContainer());
  mTrailingWhiteSpaceRange.emplace(mNBSPData.LastPointRef().NextPoint(),
                                   mEnd.PointRef());
  return mTrailingWhiteSpaceRange.ref();
}

const WSRunScanner::VisibleWhiteSpacesData&
WSRunScanner::TextFragmentData::VisibleWhiteSpacesDataRef() const {
  if (mVisibleWhiteSpacesData.isSome()) {
    return mVisibleWhiteSpacesData.ref();
  }

  if (IsPreformattedOrSurrondedByVisibleContent()) {
    VisibleWhiteSpacesData visibleWhiteSpaces;
    if (mStart.PointRef().IsSet()) {
      visibleWhiteSpaces.SetStartPoint(mStart.PointRef());
    }
    visibleWhiteSpaces.SetStartFrom(mStart.RawReason());
    if (mEnd.PointRef().IsSet()) {
      visibleWhiteSpaces.SetEndPoint(mEnd.PointRef());
    }
    visibleWhiteSpaces.SetEndBy(mEnd.RawReason());
    mVisibleWhiteSpacesData.emplace(visibleWhiteSpaces);
    return mVisibleWhiteSpacesData.ref();
  }

  // If all of the range is invisible leading or trailing white-spaces,
  // there is no visible content.
  const EditorDOMRange& leadingWhiteSpaceRange =
      InvisibleLeadingWhiteSpaceRangeRef();
  const bool maybeHaveLeadingWhiteSpaces =
      leadingWhiteSpaceRange.StartRef().IsSet() ||
      leadingWhiteSpaceRange.EndRef().IsSet();
  if (maybeHaveLeadingWhiteSpaces &&
      leadingWhiteSpaceRange.StartRef() == mStart.PointRef() &&
      leadingWhiteSpaceRange.EndRef() == mEnd.PointRef()) {
    mVisibleWhiteSpacesData.emplace(VisibleWhiteSpacesData());
    return mVisibleWhiteSpacesData.ref();
  }
  const EditorDOMRange& trailingWhiteSpaceRange =
      InvisibleTrailingWhiteSpaceRangeRef();
  const bool maybeHaveTrailingWhiteSpaces =
      trailingWhiteSpaceRange.StartRef().IsSet() ||
      trailingWhiteSpaceRange.EndRef().IsSet();
  if (maybeHaveTrailingWhiteSpaces &&
      trailingWhiteSpaceRange.StartRef() == mStart.PointRef() &&
      trailingWhiteSpaceRange.EndRef() == mEnd.PointRef()) {
    mVisibleWhiteSpacesData.emplace(VisibleWhiteSpacesData());
    return mVisibleWhiteSpacesData.ref();
  }

  if (!StartsFromHardLineBreak()) {
    VisibleWhiteSpacesData visibleWhiteSpaces;
    if (mStart.PointRef().IsSet()) {
      visibleWhiteSpaces.SetStartPoint(mStart.PointRef());
    }
    visibleWhiteSpaces.SetStartFrom(mStart.RawReason());
    if (!maybeHaveTrailingWhiteSpaces) {
      visibleWhiteSpaces.SetEndPoint(mEnd.PointRef());
      visibleWhiteSpaces.SetEndBy(mEnd.RawReason());
      mVisibleWhiteSpacesData = Some(visibleWhiteSpaces);
      return mVisibleWhiteSpacesData.ref();
    }
    if (trailingWhiteSpaceRange.StartRef().IsSet()) {
      visibleWhiteSpaces.SetEndPoint(trailingWhiteSpaceRange.StartRef());
    }
    visibleWhiteSpaces.SetEndByTrailingWhiteSpaces();
    mVisibleWhiteSpacesData.emplace(visibleWhiteSpaces);
    return mVisibleWhiteSpacesData.ref();
  }

  MOZ_ASSERT(StartsFromHardLineBreak());
  MOZ_ASSERT(maybeHaveLeadingWhiteSpaces);

  VisibleWhiteSpacesData visibleWhiteSpaces;
  if (leadingWhiteSpaceRange.EndRef().IsSet()) {
    visibleWhiteSpaces.SetStartPoint(leadingWhiteSpaceRange.EndRef());
  }
  visibleWhiteSpaces.SetStartFromLeadingWhiteSpaces();
  if (!EndsByBlockBoundary()) {
    // then no trailing ws.  this normal run ends the overall ws run.
    if (mEnd.PointRef().IsSet()) {
      visibleWhiteSpaces.SetEndPoint(mEnd.PointRef());
    }
    visibleWhiteSpaces.SetEndBy(mEnd.RawReason());
    mVisibleWhiteSpacesData.emplace(visibleWhiteSpaces);
    return mVisibleWhiteSpacesData.ref();
  }

  MOZ_ASSERT(EndsByBlockBoundary());

  if (!maybeHaveTrailingWhiteSpaces) {
    // normal ws runs right up to adjacent block (nbsp next to block)
    visibleWhiteSpaces.SetEndPoint(mEnd.PointRef());
    visibleWhiteSpaces.SetEndBy(mEnd.RawReason());
    mVisibleWhiteSpacesData.emplace(visibleWhiteSpaces);
    return mVisibleWhiteSpacesData.ref();
  }

  if (trailingWhiteSpaceRange.StartRef().IsSet()) {
    visibleWhiteSpaces.SetEndPoint(trailingWhiteSpaceRange.StartRef());
  }
  visibleWhiteSpaces.SetEndByTrailingWhiteSpaces();
  mVisibleWhiteSpacesData.emplace(visibleWhiteSpaces);
  return mVisibleWhiteSpacesData.ref();
}

// static
nsresult WhiteSpaceVisibilityKeeper::
    MakeSureToKeepVisibleStateOfWhiteSpacesAroundDeletingRange(
        HTMLEditor& aHTMLEditor, const EditorDOMRange& aRangeToDelete) {
  if (NS_WARN_IF(!aRangeToDelete.IsPositionedAndValid()) ||
      NS_WARN_IF(!aRangeToDelete.IsInContentNodes())) {
    return NS_ERROR_INVALID_ARG;
  }

  EditorDOMRange rangeToDelete(aRangeToDelete);
  bool mayBecomeUnexpectedDOMTree = aHTMLEditor.MaybeHasMutationEventListeners(
      NS_EVENT_BITS_MUTATION_SUBTREEMODIFIED |
      NS_EVENT_BITS_MUTATION_NODEREMOVED |
      NS_EVENT_BITS_MUTATION_NODEREMOVEDFROMDOCUMENT |
      NS_EVENT_BITS_MUTATION_CHARACTERDATAMODIFIED);

  RefPtr<Element> editingHost = aHTMLEditor.GetActiveEditingHost();
  TextFragmentData textFragmentDataAtStart(rangeToDelete.StartRef(),
                                           editingHost);
  TextFragmentData textFragmentDataAtEnd(rangeToDelete.EndRef(), editingHost);
  ReplaceRangeData replaceRangeDataAtEnd =
      textFragmentDataAtEnd.GetReplaceRangeDataAtEndOfDeletionRange(
          textFragmentDataAtStart);
  if (replaceRangeDataAtEnd.IsSet() && !replaceRangeDataAtEnd.Collapsed()) {
    MOZ_ASSERT(rangeToDelete.EndRef().EqualsOrIsBefore(
        replaceRangeDataAtEnd.EndRef()));
    MOZ_ASSERT_IF(rangeToDelete.EndRef().IsInTextNode(),
                  replaceRangeDataAtEnd.StartRef().EqualsOrIsBefore(
                      rangeToDelete.EndRef()));
    MOZ_ASSERT(rangeToDelete.StartRef().EqualsOrIsBefore(
        replaceRangeDataAtEnd.StartRef()));
    if (!replaceRangeDataAtEnd.HasReplaceString()) {
      EditorDOMPoint startToDelete(aRangeToDelete.StartRef());
      EditorDOMPoint endToDelete(replaceRangeDataAtEnd.StartRef());
      {
        AutoEditorDOMPointChildInvalidator lockOffsetOfStart(startToDelete);
        AutoEditorDOMPointChildInvalidator lockOffsetOfEnd(endToDelete);
        AutoTrackDOMPoint trackStartToDelete(aHTMLEditor.RangeUpdaterRef(),
                                             &startToDelete);
        AutoTrackDOMPoint trackEndToDelete(aHTMLEditor.RangeUpdaterRef(),
                                           &endToDelete);
        nsresult rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
            replaceRangeDataAtEnd.StartRef(), replaceRangeDataAtEnd.EndRef());
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
          return rv;
        }
      }
      if (mayBecomeUnexpectedDOMTree &&
          (NS_WARN_IF(!startToDelete.IsSetAndValid()) ||
           NS_WARN_IF(!endToDelete.IsSetAndValid()) ||
           NS_WARN_IF(!startToDelete.EqualsOrIsBefore(endToDelete)))) {
        return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
      }
      MOZ_ASSERT(startToDelete.EqualsOrIsBefore(endToDelete));
      rangeToDelete.SetStartAndEnd(startToDelete, endToDelete);
    } else {
      MOZ_ASSERT(replaceRangeDataAtEnd.RangeRef().IsInTextNodes());
      EditorDOMPoint startToDelete(aRangeToDelete.StartRef());
      EditorDOMPoint endToDelete(replaceRangeDataAtEnd.StartRef());
      {
        AutoTrackDOMPoint trackStartToDelete(aHTMLEditor.RangeUpdaterRef(),
                                             &startToDelete);
        AutoTrackDOMPoint trackEndToDelete(aHTMLEditor.RangeUpdaterRef(),
                                           &endToDelete);
        nsresult rv =
            WhiteSpaceVisibilityKeeper::ReplaceTextAndRemoveEmptyTextNodes(
                aHTMLEditor, replaceRangeDataAtEnd.RangeRef().AsInTexts(),
                replaceRangeDataAtEnd.ReplaceStringRef());
        if (NS_FAILED(rv)) {
          NS_WARNING(
              "WhiteSpaceVisibilityKeeper::"
              "MakeSureToKeepVisibleStateOfWhiteSpacesAtEndOfDeletingRange() "
              "failed");
          return rv;
        }
      }
      if (mayBecomeUnexpectedDOMTree &&
          (NS_WARN_IF(!startToDelete.IsSetAndValid()) ||
           NS_WARN_IF(!endToDelete.IsSetAndValid()) ||
           NS_WARN_IF(!startToDelete.EqualsOrIsBefore(endToDelete)))) {
        return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
      }
      MOZ_ASSERT(startToDelete.EqualsOrIsBefore(endToDelete));
      rangeToDelete.SetStartAndEnd(startToDelete, endToDelete);
    }

    if (mayBecomeUnexpectedDOMTree) {
      // If focus is changed by mutation event listeners, we should stop
      // handling this edit action.
      if (editingHost != aHTMLEditor.GetActiveEditingHost()) {
        NS_WARNING("Active editing host was changed");
        return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
      }
      if (!rangeToDelete.IsInContentNodes()) {
        NS_WARNING("The modified range was not in content");
        return NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE;
      }
      // If the DOM tree might be changed by mutation event listeners, we
      // should retrieve the latest data for avoiding to delete/replace
      // unexpected range.
      textFragmentDataAtStart =
          TextFragmentData(rangeToDelete.StartRef(), editingHost);
      textFragmentDataAtEnd =
          TextFragmentData(rangeToDelete.EndRef(), editingHost);
    }
  }
  ReplaceRangeData replaceRangeDataAtStart =
      textFragmentDataAtStart.GetReplaceRangeDataAtStartOfDeletionRange(
          textFragmentDataAtEnd);
  if (!replaceRangeDataAtStart.IsSet() || replaceRangeDataAtStart.Collapsed()) {
    return NS_OK;
  }
  if (!replaceRangeDataAtStart.HasReplaceString()) {
    nsresult rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
        replaceRangeDataAtStart.StartRef(), replaceRangeDataAtStart.EndRef());
    // XXX Should we validate the range for making this return
    //     NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE in this case?
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
    return rv;
  }
  MOZ_ASSERT(replaceRangeDataAtStart.RangeRef().IsInTextNodes());
  nsresult rv = WhiteSpaceVisibilityKeeper::ReplaceTextAndRemoveEmptyTextNodes(
      aHTMLEditor, replaceRangeDataAtStart.RangeRef().AsInTexts(),
      replaceRangeDataAtStart.ReplaceStringRef());
  // XXX Should we validate the range for making this return
  //     NS_ERROR_EDITOR_UNEXPECTED_DOM_TREE in this case?
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "WhiteSpaceVisibilityKeeper::"
      "MakeSureToKeepVisibleStateOfWhiteSpacesAtStartOfDeletingRange() failed");
  return rv;
}

ReplaceRangeData
WSRunScanner::TextFragmentData::GetReplaceRangeDataAtEndOfDeletionRange(
    const TextFragmentData& aTextFragmentDataAtStartToDelete) {
  const EditorDOMPoint& startToDelete =
      aTextFragmentDataAtStartToDelete.ScanStartRef();
  const EditorDOMPoint& endToDelete = mScanStartPoint;

  MOZ_ASSERT(startToDelete.IsSetAndValid());
  MOZ_ASSERT(endToDelete.IsSetAndValid());
  MOZ_ASSERT(startToDelete.EqualsOrIsBefore(endToDelete));

  if (EndRef().EqualsOrIsBefore(endToDelete)) {
    return ReplaceRangeData();
  }

  // If deleting range is followed by invisible trailing white-spaces, we need
  // to remove it for making them not visible.
  const EditorDOMRange invisibleTrailingWhiteSpaceRangeAtEnd =
      GetNewInvisibleTrailingWhiteSpaceRangeIfSplittingAt(endToDelete);
  if (invisibleTrailingWhiteSpaceRangeAtEnd.IsPositioned()) {
    if (invisibleTrailingWhiteSpaceRangeAtEnd.Collapsed()) {
      return ReplaceRangeData();
    }
    // XXX Why don't we remove all invisible white-spaces?
    MOZ_ASSERT(invisibleTrailingWhiteSpaceRangeAtEnd.StartRef() == endToDelete);
    return ReplaceRangeData(invisibleTrailingWhiteSpaceRangeAtEnd,
                            EmptyString());
  }

  if (IsPreformatted()) {
    return ReplaceRangeData();
  }

  // If end of the deleting range is followed by visible white-spaces which
  // is not preformatted, we might need to replace the following ASCII
  // white-spaces with an NBSP.
  const VisibleWhiteSpacesData& nonPreformattedVisibleWhiteSpacesAtEnd =
      VisibleWhiteSpacesDataRef();
  if (!nonPreformattedVisibleWhiteSpacesAtEnd.IsInitialized()) {
    return ReplaceRangeData();
  }
  const PointPosition pointPositionWithNonPreformattedVisibleWhiteSpacesAtEnd =
      nonPreformattedVisibleWhiteSpacesAtEnd.ComparePoint(endToDelete);
  if (pointPositionWithNonPreformattedVisibleWhiteSpacesAtEnd !=
          PointPosition::StartOfFragment &&
      pointPositionWithNonPreformattedVisibleWhiteSpacesAtEnd !=
          PointPosition::MiddleOfFragment) {
    return ReplaceRangeData();
  }
  // If start of deleting range follows white-spaces or end of delete
  // will be start of a line, the following text cannot start with an
  // ASCII white-space for keeping it visible.
  if (!aTextFragmentDataAtStartToDelete
           .FollowingContentMayBecomeFirstVisibleContent(startToDelete)) {
    return ReplaceRangeData();
  }
  EditorRawDOMPointInText nextCharOfStartOfEnd =
      GetInclusiveNextEditableCharPoint(endToDelete);
  if (!nextCharOfStartOfEnd.IsSet() ||
      nextCharOfStartOfEnd.IsEndOfContainer() ||
      !nextCharOfStartOfEnd.IsCharASCIISpace() ||
      EditorUtils::IsContentPreformatted(
          *nextCharOfStartOfEnd.ContainerAsText())) {
    return ReplaceRangeData();
  }
  if (nextCharOfStartOfEnd.IsStartOfContainer() ||
      nextCharOfStartOfEnd.IsPreviousCharASCIISpace()) {
    nextCharOfStartOfEnd =
        aTextFragmentDataAtStartToDelete
            .GetFirstASCIIWhiteSpacePointCollapsedTo(nextCharOfStartOfEnd);
  }
  EditorRawDOMPointInText endOfCollapsibleASCIIWhiteSpaces =
      aTextFragmentDataAtStartToDelete.GetEndOfCollapsibleASCIIWhiteSpaces(
          nextCharOfStartOfEnd);
  return ReplaceRangeData(nextCharOfStartOfEnd,
                          endOfCollapsibleASCIIWhiteSpaces,
                          nsDependentSubstring(&kNBSP, 1));
}

ReplaceRangeData
WSRunScanner::TextFragmentData::GetReplaceRangeDataAtStartOfDeletionRange(
    const TextFragmentData& aTextFragmentDataAtEndToDelete) {
  const EditorDOMPoint& startToDelete = mScanStartPoint;
  const EditorDOMPoint& endToDelete =
      aTextFragmentDataAtEndToDelete.ScanStartRef();

  MOZ_ASSERT(startToDelete.IsSetAndValid());
  MOZ_ASSERT(endToDelete.IsSetAndValid());
  MOZ_ASSERT(startToDelete.EqualsOrIsBefore(endToDelete));

  if (startToDelete.EqualsOrIsBefore(StartRef())) {
    return ReplaceRangeData();
  }

  const EditorDOMRange invisibleLeadingWhiteSpaceRangeAtStart =
      GetNewInvisibleLeadingWhiteSpaceRangeIfSplittingAt(startToDelete);

  // If deleting range follows invisible leading white-spaces, we need to
  // remove them for making them not visible.
  if (invisibleLeadingWhiteSpaceRangeAtStart.IsPositioned()) {
    if (invisibleLeadingWhiteSpaceRangeAtStart.Collapsed()) {
      return ReplaceRangeData();
    }

    // XXX Why don't we remove all leading white-spaces?
    return ReplaceRangeData(invisibleLeadingWhiteSpaceRangeAtStart,
                            EmptyString());
  }

  if (IsPreformatted()) {
    return ReplaceRangeData();
  }

  // If start of the deleting range follows visible white-spaces which is not
  // preformatted, we might need to replace previous ASCII white-spaces with
  // an NBSP.
  const VisibleWhiteSpacesData& nonPreformattedVisibleWhiteSpacesAtStart =
      VisibleWhiteSpacesDataRef();
  if (!nonPreformattedVisibleWhiteSpacesAtStart.IsInitialized()) {
    return ReplaceRangeData();
  }
  const PointPosition
      pointPositionWithNonPreformattedVisibleWhiteSpacesAtStart =
          nonPreformattedVisibleWhiteSpacesAtStart.ComparePoint(startToDelete);
  if (pointPositionWithNonPreformattedVisibleWhiteSpacesAtStart !=
          PointPosition::MiddleOfFragment &&
      pointPositionWithNonPreformattedVisibleWhiteSpacesAtStart !=
          PointPosition::EndOfFragment) {
    return ReplaceRangeData();
  }
  // If end of the deleting range is (was) followed by white-spaces or
  // previous character of start of deleting range will be immediately
  // before a block boundary, the text cannot ends with an ASCII white-space
  // for keeping it visible.
  if (!aTextFragmentDataAtEndToDelete.PrecedingContentMayBecomeInvisible(
          endToDelete)) {
    return ReplaceRangeData();
  }
  EditorRawDOMPointInText atPreviousCharOfStart =
      GetPreviousEditableCharPoint(startToDelete);
  if (!atPreviousCharOfStart.IsSet() ||
      atPreviousCharOfStart.IsEndOfContainer() ||
      !atPreviousCharOfStart.IsCharASCIISpace() ||
      EditorUtils::IsContentPreformatted(
          *atPreviousCharOfStart.ContainerAsText())) {
    return ReplaceRangeData();
  }
  if (atPreviousCharOfStart.IsStartOfContainer() ||
      atPreviousCharOfStart.IsPreviousCharASCIISpace()) {
    atPreviousCharOfStart =
        GetFirstASCIIWhiteSpacePointCollapsedTo(atPreviousCharOfStart);
  }
  EditorRawDOMPointInText endOfCollapsibleASCIIWhiteSpaces =
      GetEndOfCollapsibleASCIIWhiteSpaces(atPreviousCharOfStart);
  return ReplaceRangeData(atPreviousCharOfStart,
                          endOfCollapsibleASCIIWhiteSpaces,
                          nsDependentSubstring(&kNBSP, 1));
}

// static
nsresult
WhiteSpaceVisibilityKeeper::MakeSureToKeepVisibleWhiteSpacesVisibleAfterSplit(
    HTMLEditor& aHTMLEditor, const EditorDOMPoint& aPointToSplit) {
  TextFragmentData textFragmentDataAtSplitPoint(
      aPointToSplit, aHTMLEditor.GetActiveEditingHost());

  // used to prepare white-space sequence to be split across two blocks.
  // The main issue here is make sure white-spaces around the split point
  // doesn't end up becoming non-significant leading or trailing ws after
  // the split.
  const VisibleWhiteSpacesData& visibleWhiteSpaces =
      textFragmentDataAtSplitPoint.VisibleWhiteSpacesDataRef();
  if (!visibleWhiteSpaces.IsInitialized()) {
    return NS_OK;  // No visible white-space sequence.
  }

  PointPosition pointPositionWithVisibleWhiteSpaces =
      visibleWhiteSpaces.ComparePoint(aPointToSplit);

  // XXX If we split white-space sequence, the following code modify the DOM
  //     tree twice.  This is not reasonable and the latter change may touch
  //     wrong position.  We should do this once.

  // If we insert block boundary to start or middle of the white-space sequence,
  // the character at the insertion point needs to be an NBSP.
  EditorDOMPoint pointToSplit(aPointToSplit);
  if (pointPositionWithVisibleWhiteSpaces == PointPosition::StartOfFragment ||
      pointPositionWithVisibleWhiteSpaces == PointPosition::MiddleOfFragment) {
    EditorRawDOMPointInText atNextCharOfStart =
        textFragmentDataAtSplitPoint.GetInclusiveNextEditableCharPoint(
            pointToSplit);
    if (atNextCharOfStart.IsSet() && !atNextCharOfStart.IsEndOfContainer() &&
        atNextCharOfStart.IsCharASCIISpace() &&
        !EditorUtils::IsContentPreformatted(
            *atNextCharOfStart.ContainerAsText())) {
      // pointToSplit will be referred bellow so that we need to keep
      // it a valid point.
      AutoEditorDOMPointChildInvalidator forgetChild(pointToSplit);
      if (atNextCharOfStart.IsStartOfContainer() ||
          atNextCharOfStart.IsPreviousCharASCIISpace()) {
        atNextCharOfStart =
            textFragmentDataAtSplitPoint
                .GetFirstASCIIWhiteSpacePointCollapsedTo(atNextCharOfStart);
      }
      EditorRawDOMPointInText endOfCollapsibleASCIIWhiteSpaces =
          textFragmentDataAtSplitPoint.GetEndOfCollapsibleASCIIWhiteSpaces(
              atNextCharOfStart);
      nsresult rv =
          WhiteSpaceVisibilityKeeper::ReplaceTextAndRemoveEmptyTextNodes(
              aHTMLEditor,
              EditorDOMRangeInTexts(atNextCharOfStart,
                                    endOfCollapsibleASCIIWhiteSpaces),
              nsDependentSubstring(&kNBSP, 1));
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "WhiteSpaceVisibilityKeeper::ReplaceTextAndRemoveEmptyTextNodes() "
            "failed");
        return rv;
      }
    }
  }

  // If we insert block boundary to middle of or end of the white-space
  // sequence, the previous character at the insertion point needs to be an
  // NBSP.
  if (pointPositionWithVisibleWhiteSpaces == PointPosition::MiddleOfFragment ||
      pointPositionWithVisibleWhiteSpaces == PointPosition::EndOfFragment) {
    EditorRawDOMPointInText atPreviousCharOfStart =
        textFragmentDataAtSplitPoint.GetPreviousEditableCharPoint(pointToSplit);
    if (atPreviousCharOfStart.IsSet() &&
        !atPreviousCharOfStart.IsEndOfContainer() &&
        atPreviousCharOfStart.IsCharASCIISpace() &&
        !EditorUtils::IsContentPreformatted(
            *atPreviousCharOfStart.ContainerAsText())) {
      if (atPreviousCharOfStart.IsStartOfContainer() ||
          atPreviousCharOfStart.IsPreviousCharASCIISpace()) {
        atPreviousCharOfStart =
            textFragmentDataAtSplitPoint
                .GetFirstASCIIWhiteSpacePointCollapsedTo(atPreviousCharOfStart);
      }
      EditorRawDOMPointInText endOfCollapsibleASCIIWhiteSpaces =
          textFragmentDataAtSplitPoint.GetEndOfCollapsibleASCIIWhiteSpaces(
              atPreviousCharOfStart);
      nsresult rv =
          WhiteSpaceVisibilityKeeper::ReplaceTextAndRemoveEmptyTextNodes(
              aHTMLEditor,
              EditorDOMRangeInTexts(atPreviousCharOfStart,
                                    endOfCollapsibleASCIIWhiteSpaces),
              nsDependentSubstring(&kNBSP, 1));
      if (NS_FAILED(rv)) {
        NS_WARNING(
            "WhiteSpaceVisibilityKeeper::ReplaceTextAndRemoveEmptyTextNodes() "
            "failed");
        return rv;
      }
    }
  }
  return NS_OK;
}

template <typename PT, typename CT>
EditorDOMPointInText
WSRunScanner::TextFragmentData::GetInclusiveNextEditableCharPoint(
    const EditorDOMPointBase<PT, CT>& aPoint) const {
  MOZ_ASSERT(aPoint.IsSetAndValid());

  if (NS_WARN_IF(!aPoint.IsInContentNode()) ||
      NS_WARN_IF(!mScanStartPoint.IsInContentNode())) {
    return EditorDOMPointInText();
  }

  EditorRawDOMPoint point;
  if (nsIContent* child =
          aPoint.CanContainerHaveChildren() ? aPoint.GetChild() : nullptr) {
    nsIContent* leafContent = child->HasChildren()
                                  ? HTMLEditUtils::GetFirstLeafChild(
                                        *child, ChildBlockBoundary::Ignore)
                                  : child;
    if (NS_WARN_IF(!leafContent)) {
      return EditorDOMPointInText();
    }
    point.Set(leafContent, 0);
  } else {
    point = aPoint;
  }

  // If it points a character in a text node, return it.
  // XXX For the performance, this does not check whether the container
  //     is outside of our range.
  if (point.IsInTextNode() && point.GetContainer()->IsEditable() &&
      !point.IsEndOfContainer()) {
    return EditorDOMPointInText(point.ContainerAsText(), point.Offset());
  }

  if (point.GetContainer() == GetEndReasonContent()) {
    return EditorDOMPointInText();
  }

  NS_ASSERTION(EditorUtils::IsEditableContent(
                   *mScanStartPoint.ContainerAsContent(), EditorType::HTML),
               "Given content is not editable");
  NS_ASSERTION(
      mScanStartPoint.ContainerAsContent()->GetAsElementOrParentElement(),
      "Given content is not an element and an orphan node");
  nsIContent* editableBlockParentOrTopmotEditableInlineContent =
      mScanStartPoint.ContainerAsContent() &&
              EditorUtils::IsEditableContent(
                  *mScanStartPoint.ContainerAsContent(), EditorType::HTML)
          ? HTMLEditUtils::
                GetInclusiveAncestorEditableBlockElementOrInlineEditingHost(
                    *mScanStartPoint.ContainerAsContent())
          : nullptr;
  if (NS_WARN_IF(!editableBlockParentOrTopmotEditableInlineContent)) {
    // Meaning that the container of `mScanStartPoint` is not editable.
    editableBlockParentOrTopmotEditableInlineContent =
        mScanStartPoint.ContainerAsContent();
  }

  for (nsIContent* nextContent =
           HTMLEditUtils::GetNextLeafContentOrNextBlockElement(
               *point.ContainerAsContent(),
               *editableBlockParentOrTopmotEditableInlineContent, mEditingHost);
       nextContent;
       nextContent = HTMLEditUtils::GetNextLeafContentOrNextBlockElement(
           *nextContent, *editableBlockParentOrTopmotEditableInlineContent,
           mEditingHost)) {
    if (!nextContent->IsText() || !nextContent->IsEditable()) {
      if (nextContent == GetEndReasonContent()) {
        break;  // Reached end of current runs.
      }
      continue;
    }
    return EditorDOMPointInText(nextContent->AsText(), 0);
  }
  return EditorDOMPointInText();
}

template <typename PT, typename CT>
EditorDOMPointInText
WSRunScanner::TextFragmentData::GetPreviousEditableCharPoint(
    const EditorDOMPointBase<PT, CT>& aPoint) const {
  MOZ_ASSERT(aPoint.IsSetAndValid());

  if (NS_WARN_IF(!aPoint.IsInContentNode()) ||
      NS_WARN_IF(!mScanStartPoint.IsInContentNode())) {
    return EditorDOMPointInText();
  }

  EditorRawDOMPoint point;
  if (nsIContent* previousChild = aPoint.CanContainerHaveChildren()
                                      ? aPoint.GetPreviousSiblingOfChild()
                                      : nullptr) {
    nsIContent* leafContent =
        previousChild->HasChildren()
            ? HTMLEditUtils::GetLastLeafChild(*previousChild,
                                              ChildBlockBoundary::Ignore)
            : previousChild;
    if (NS_WARN_IF(!leafContent)) {
      return EditorDOMPointInText();
    }
    point.SetToEndOf(leafContent);
  } else {
    point = aPoint;
  }

  // If it points a character in a text node and it's not first character
  // in it, return its previous point.
  // XXX For the performance, this does not check whether the container
  //     is outside of our range.
  if (point.IsInTextNode() && point.GetContainer()->IsEditable() &&
      !point.IsStartOfContainer()) {
    return EditorDOMPointInText(point.ContainerAsText(), point.Offset() - 1);
  }

  if (point.GetContainer() == GetStartReasonContent()) {
    return EditorDOMPointInText();
  }

  NS_ASSERTION(EditorUtils::IsEditableContent(
                   *mScanStartPoint.ContainerAsContent(), EditorType::HTML),
               "Given content is not editable");
  NS_ASSERTION(
      mScanStartPoint.ContainerAsContent()->GetAsElementOrParentElement(),
      "Given content is not an element and an orphan node");
  nsIContent* editableBlockParentOrTopmotEditableInlineContent =
      mScanStartPoint.ContainerAsContent() &&
              EditorUtils::IsEditableContent(
                  *mScanStartPoint.ContainerAsContent(), EditorType::HTML)
          ? HTMLEditUtils::
                GetInclusiveAncestorEditableBlockElementOrInlineEditingHost(
                    *mScanStartPoint.ContainerAsContent())
          : nullptr;
  if (NS_WARN_IF(!editableBlockParentOrTopmotEditableInlineContent)) {
    // Meaning that the container of `mScanStartPoint` is not editable.
    editableBlockParentOrTopmotEditableInlineContent =
        mScanStartPoint.ContainerAsContent();
  }

  for (nsIContent* previousContent =
           HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
               *point.ContainerAsContent(),
               *editableBlockParentOrTopmotEditableInlineContent, mEditingHost);
       previousContent;
       previousContent =
           HTMLEditUtils::GetPreviousLeafContentOrPreviousBlockElement(
               *previousContent,
               *editableBlockParentOrTopmotEditableInlineContent,
               mEditingHost)) {
    if (!previousContent->IsText() || !previousContent->IsEditable()) {
      if (previousContent == GetStartReasonContent()) {
        break;  // Reached start of current runs.
      }
      continue;
    }
    return EditorDOMPointInText(
        previousContent->AsText(),
        previousContent->AsText()->TextLength()
            ? previousContent->AsText()->TextLength() - 1
            : 0);
  }
  return EditorDOMPointInText();
}

EditorDOMPointInText
WSRunScanner::TextFragmentData::GetEndOfCollapsibleASCIIWhiteSpaces(
    const EditorDOMPointInText& aPointAtASCIIWhiteSpace) const {
  MOZ_ASSERT(aPointAtASCIIWhiteSpace.IsSet());
  MOZ_ASSERT(!aPointAtASCIIWhiteSpace.IsEndOfContainer());
  MOZ_ASSERT(aPointAtASCIIWhiteSpace.IsCharASCIISpace());
  NS_ASSERTION(!EditorUtils::IsContentPreformatted(
                   *aPointAtASCIIWhiteSpace.ContainerAsText()),
               "aPointAtASCIIWhiteSpace should be in a formatted text node");

  // If it's not the last character in the text node, let's scan following
  // characters in it.
  if (!aPointAtASCIIWhiteSpace.IsAtLastContent()) {
    Maybe<uint32_t> nextVisibleCharOffset =
        HTMLEditUtils::GetNextCharOffsetExceptASCIIWhiteSpaces(
            aPointAtASCIIWhiteSpace);
    if (nextVisibleCharOffset.isSome()) {
      // There is non-white-space character in it.
      return EditorDOMPointInText(aPointAtASCIIWhiteSpace.ContainerAsText(),
                                  nextVisibleCharOffset.value());
    }
  }

  // Otherwise, i.e., the text node ends with ASCII white-space, keep scanning
  // the following text nodes.
  // XXX Perhaps, we should stop scanning if there is non-editable and visible
  //     content.
  EditorDOMPointInText afterLastWhiteSpace =
      EditorDOMPointInText::AtEndOf(*aPointAtASCIIWhiteSpace.ContainerAsText());
  for (EditorDOMPointInText atEndOfPreviousTextNode = afterLastWhiteSpace;;) {
    EditorDOMPointInText atStartOfNextTextNode =
        GetInclusiveNextEditableCharPoint(atEndOfPreviousTextNode);
    if (!atStartOfNextTextNode.IsSet()) {
      // There is no more text nodes.  Return end of the previous text node.
      return afterLastWhiteSpace;
    }

    // We can ignore empty text nodes (even if it's preformatted).
    if (atStartOfNextTextNode.IsContainerEmpty()) {
      atEndOfPreviousTextNode = atStartOfNextTextNode;
      continue;
    }

    // If next node starts with non-white-space character or next node is
    // preformatted, return end of previous text node.
    if (!atStartOfNextTextNode.IsCharASCIISpace() ||
        EditorUtils::IsContentPreformatted(
            *atStartOfNextTextNode.ContainerAsText())) {
      return afterLastWhiteSpace;
    }

    // Otherwise, scan the text node.
    Maybe<uint32_t> nextVisibleCharOffset =
        HTMLEditUtils::GetNextCharOffsetExceptASCIIWhiteSpaces(
            atStartOfNextTextNode);
    if (nextVisibleCharOffset.isSome()) {
      return EditorDOMPointInText(atStartOfNextTextNode.ContainerAsText(),
                                  nextVisibleCharOffset.value());
    }

    // The next text nodes ends with white-space too.  Try next one.
    afterLastWhiteSpace = atEndOfPreviousTextNode =
        EditorDOMPointInText::AtEndOf(*atStartOfNextTextNode.ContainerAsText());
  }
}

EditorDOMPointInText
WSRunScanner::TextFragmentData::GetFirstASCIIWhiteSpacePointCollapsedTo(
    const EditorDOMPointInText& aPointAtASCIIWhiteSpace) const {
  MOZ_ASSERT(aPointAtASCIIWhiteSpace.IsSet());
  MOZ_ASSERT(!aPointAtASCIIWhiteSpace.IsEndOfContainer());
  MOZ_ASSERT(aPointAtASCIIWhiteSpace.IsCharASCIISpace());
  NS_ASSERTION(!EditorUtils::IsContentPreformatted(
                   *aPointAtASCIIWhiteSpace.ContainerAsText()),
               "aPointAtASCIIWhiteSpace should be in a formatted text node");

  // If there is some characters before it, scan it in the text node first.
  if (!aPointAtASCIIWhiteSpace.IsStartOfContainer()) {
    uint32_t firstASCIIWhiteSpaceOffset =
        HTMLEditUtils::GetFirstASCIIWhiteSpaceOffsetCollapsedWith(
            aPointAtASCIIWhiteSpace);
    if (firstASCIIWhiteSpaceOffset) {
      // There is a non-white-space character in it.
      return EditorDOMPointInText(aPointAtASCIIWhiteSpace.ContainerAsText(),
                                  firstASCIIWhiteSpaceOffset);
    }
  }

  // Otherwise, i.e., the text node starts with ASCII white-space, keep scanning
  // the preceding text nodes.
  // XXX Perhaps, we should stop scanning if there is non-editable and visible
  //     content.
  EditorDOMPointInText atLastWhiteSpace =
      EditorDOMPointInText(aPointAtASCIIWhiteSpace.ContainerAsText(), 0);
  for (EditorDOMPointInText atStartOfPreviousTextNode = atLastWhiteSpace;;) {
    EditorDOMPointInText atLastCharOfNextTextNode =
        GetPreviousEditableCharPoint(atStartOfPreviousTextNode);
    if (!atLastCharOfNextTextNode.IsSet()) {
      // There is no more text nodes.  Return end of last text node.
      return atLastWhiteSpace;
    }

    // We can ignore empty text nodes (even if it's preformatted).
    if (atLastCharOfNextTextNode.IsContainerEmpty()) {
      atStartOfPreviousTextNode = atLastCharOfNextTextNode;
      continue;
    }

    // If next node ends with non-white-space character or next node is
    // preformatted, return start of previous text node.
    if (!atLastCharOfNextTextNode.IsCharASCIISpace() ||
        EditorUtils::IsContentPreformatted(
            *atLastCharOfNextTextNode.ContainerAsText())) {
      return atLastWhiteSpace;
    }

    // Otherwise, scan the text node.
    uint32_t firstASCIIWhiteSpaceOffset =
        HTMLEditUtils::GetFirstASCIIWhiteSpaceOffsetCollapsedWith(
            atLastCharOfNextTextNode);
    if (firstASCIIWhiteSpaceOffset) {
      return EditorDOMPointInText(atLastCharOfNextTextNode.ContainerAsText(),
                                  firstASCIIWhiteSpaceOffset);
    }

    // The next text nodes starts with white-space too.  Try next one.
    atLastWhiteSpace = atStartOfPreviousTextNode =
        EditorDOMPointInText(atLastCharOfNextTextNode.ContainerAsText(), 0);
  }
}

// static
nsresult WhiteSpaceVisibilityKeeper::ReplaceTextAndRemoveEmptyTextNodes(
    HTMLEditor& aHTMLEditor, const EditorDOMRangeInTexts& aRangeToReplace,
    const nsAString& aReplaceString) {
  MOZ_ASSERT(aRangeToReplace.IsPositioned());
  MOZ_ASSERT(aRangeToReplace.StartRef().IsSetAndValid());
  MOZ_ASSERT(aRangeToReplace.EndRef().IsSetAndValid());
  MOZ_ASSERT(aRangeToReplace.StartRef().IsBefore(aRangeToReplace.EndRef()));

  AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
  nsresult rv = aHTMLEditor.ReplaceTextWithTransaction(
      MOZ_KnownLive(*aRangeToReplace.StartRef().ContainerAsText()),
      aRangeToReplace.StartRef().Offset(),
      aRangeToReplace.InSameContainer()
          ? aRangeToReplace.EndRef().Offset() -
                aRangeToReplace.StartRef().Offset()
          : aRangeToReplace.StartRef().ContainerAsText()->TextLength() -
                aRangeToReplace.StartRef().Offset(),
      aReplaceString);
  if (NS_FAILED(rv)) {
    NS_WARNING("HTMLEditor::ReplaceTextWithTransaction() failed");
    return rv;
  }

  if (aRangeToReplace.InSameContainer()) {
    return NS_OK;
  }

  rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
      EditorDOMPointInText::AtEndOf(
          *aRangeToReplace.StartRef().ContainerAsText()),
      aRangeToReplace.EndRef());
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
  return rv;
}

char16_t WSRunScanner::GetCharAt(Text* aTextNode, int32_t aOffset) const {
  // return 0 if we can't get a char, for whatever reason
  if (NS_WARN_IF(!aTextNode) || NS_WARN_IF(aOffset < 0) ||
      NS_WARN_IF(aOffset >=
                 static_cast<int32_t>(aTextNode->TextDataLength()))) {
    return 0;
  }
  return aTextNode->TextFragment().CharAt(aOffset);
}

// static
template <typename EditorDOMPointType>
nsresult WhiteSpaceVisibilityKeeper::NormalizeVisibleWhiteSpacesAt(
    HTMLEditor& aHTMLEditor, const EditorDOMPointType& aPoint) {
  Element* editingHost = aHTMLEditor.GetActiveEditingHost();
  TextFragmentData textFragmentData(aPoint, editingHost);
  // this routine examines a run of ws and tries to get rid of some unneeded
  // nbsp's, replacing them with regular ascii space if possible.  Keeping
  // things simple for now and just trying to fix up the trailing ws in the run.
  if (!textFragmentData.FoundNoBreakingWhiteSpaces()) {
    // nothing to do!
    return NS_OK;
  }
  const VisibleWhiteSpacesData& visibleWhiteSpaces =
      textFragmentData.VisibleWhiteSpacesDataRef();
  if (!visibleWhiteSpaces.IsInitialized()) {
    return NS_OK;
  }

  // Remove this block if we ship Blink-compat white-space normalization.
  if (!StaticPrefs::editor_white_space_normalization_blink_compatible()) {
    // now check that what is to the left of it is compatible with replacing
    // nbsp with space
    const EditorDOMPoint& atEndOfVisibleWhiteSpaces =
        visibleWhiteSpaces.EndRef();
    EditorDOMPointInText atPreviousCharOfEndOfVisibleWhiteSpaces =
        textFragmentData.GetPreviousEditableCharPoint(
            atEndOfVisibleWhiteSpaces);
    if (!atPreviousCharOfEndOfVisibleWhiteSpaces.IsSet() ||
        atPreviousCharOfEndOfVisibleWhiteSpaces.IsEndOfContainer() ||
        !atPreviousCharOfEndOfVisibleWhiteSpaces.IsCharNBSP()) {
      return NS_OK;
    }

    // now check that what is to the left of it is compatible with replacing
    // nbsp with space
    EditorDOMPointInText atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces =
        textFragmentData.GetPreviousEditableCharPoint(
            atPreviousCharOfEndOfVisibleWhiteSpaces);
    bool isPreviousCharASCIIWhiteSpace =
        atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces.IsSet() &&
        !atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces
             .IsEndOfContainer() &&
        atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces
            .IsCharASCIISpace();
    bool maybeNBSPFollowingVisibleContent =
        (atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces.IsSet() &&
         !isPreviousCharASCIIWhiteSpace) ||
        (!atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces.IsSet() &&
         (visibleWhiteSpaces.StartsFromNormalText() ||
          visibleWhiteSpaces.StartsFromSpecialContent()));
    bool followedByVisibleContentOrBRElement = false;

    // If the NBSP follows a visible content or an ASCII white-space, i.e.,
    // unless NBSP is first character and start of a block, we may need to
    // insert <br> element and restore the NBSP to an ASCII white-space.
    if (maybeNBSPFollowingVisibleContent || isPreviousCharASCIIWhiteSpace) {
      followedByVisibleContentOrBRElement =
          visibleWhiteSpaces.EndsByNormalText() ||
          visibleWhiteSpaces.EndsBySpecialContent() ||
          visibleWhiteSpaces.EndsByBRElement();
      // First, try to insert <br> element if NBSP is at end of a block.
      // XXX We should stop this if there is a visible content.
      if (visibleWhiteSpaces.EndsByBlockBoundary() &&
          aPoint.IsInContentNode()) {
        bool insertBRElement =
            HTMLEditUtils::IsBlockElement(*aPoint.ContainerAsContent());
        if (!insertBRElement) {
          NS_ASSERTION(EditorUtils::IsEditableContent(
                           *aPoint.ContainerAsContent(), EditorType::HTML),
                       "Given content is not editable");
          NS_ASSERTION(
              aPoint.ContainerAsContent()->GetAsElementOrParentElement(),
              "Given content is not an element and an orphan node");
          nsIContent* blockParentOrTopmostEditableInlineContent =
              EditorUtils::IsEditableContent(*aPoint.ContainerAsContent(),
                                             EditorType::HTML)
                  ? HTMLEditUtils::
                        GetInclusiveAncestorEditableBlockElementOrInlineEditingHost(
                            *aPoint.ContainerAsContent())
                  : nullptr;
          insertBRElement = blockParentOrTopmostEditableInlineContent &&
                            HTMLEditUtils::IsBlockElement(
                                *blockParentOrTopmostEditableInlineContent);
        }
        if (insertBRElement) {
          // We are at a block boundary.  Insert a <br>.  Why?  Well, first note
          // that the br will have no visible effect since it is up against a
          // block boundary.  |foo<br><p>bar| renders like |foo<p>bar| and
          // similarly |<p>foo<br></p>bar| renders like |<p>foo</p>bar|.  What
          // this <br> addition gets us is the ability to convert a trailing
          // nbsp to a space.  Consider: |<body>foo. '</body>|, where '
          // represents selection.  User types space attempting to put 2 spaces
          // after the end of their sentence.  We used to do this as:
          // |<body>foo. &nbsp</body>|  This caused problems with soft wrapping:
          // the nbsp would wrap to the next line, which looked attrocious.  If
          // you try to do: |<body>foo.&nbsp </body>| instead, the trailing
          // space is invisible because it is against a block boundary.  If you
          // do:
          // |<body>foo.&nbsp&nbsp</body>| then you get an even uglier soft
          // wrapping problem, where foo is on one line until you type the final
          // space, and then "foo  " jumps down to the next line.  Ugh.  The
          // best way I can find out of this is to throw in a harmless <br>
          // here, which allows us to do: |<body>foo.&nbsp <br></body>|, which
          // doesn't cause foo to jump lines, doesn't cause spaces to show up at
          // the beginning of soft wrapped lines, and lets the user see 2 spaces
          // when they type 2 spaces.

          RefPtr<Element> brElement =
              aHTMLEditor.InsertBRElementWithTransaction(
                  atEndOfVisibleWhiteSpaces);
          if (NS_WARN_IF(aHTMLEditor.Destroyed())) {
            return NS_ERROR_EDITOR_DESTROYED;
          }
          if (!brElement) {
            NS_WARNING("HTMLEditor::InsertBRElementWithTransaction() failed");
            return NS_ERROR_FAILURE;
          }

          atPreviousCharOfEndOfVisibleWhiteSpaces =
              textFragmentData.GetPreviousEditableCharPoint(
                  atEndOfVisibleWhiteSpaces);
          atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces =
              textFragmentData.GetPreviousEditableCharPoint(
                  atPreviousCharOfEndOfVisibleWhiteSpaces);
          isPreviousCharASCIIWhiteSpace =
              atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces.IsSet() &&
              !atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces
                   .IsEndOfContainer() &&
              atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces
                  .IsCharASCIISpace();
          followedByVisibleContentOrBRElement = true;
        }
      }

      // Next, replace the NBSP with an ASCII white-space if it's surrounded
      // by visible contents (or immediately before a <br> element).
      if (maybeNBSPFollowingVisibleContent &&
          followedByVisibleContentOrBRElement) {
        AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
        nsresult rv = aHTMLEditor.ReplaceTextWithTransaction(
            MOZ_KnownLive(
                *atPreviousCharOfEndOfVisibleWhiteSpaces.ContainerAsText()),
            atPreviousCharOfEndOfVisibleWhiteSpaces.Offset(), 1, u" "_ns);
        NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                             "HTMLEditor::ReplaceTextWithTransaction() failed");
        return rv;
      }
    }
    // If the text node is not preformatted, and the NBSP is followed by a <br>
    // element and following (maybe multiple) ASCII spaces, remove the NBSP,
    // but inserts a NBSP before the spaces.  This makes a line break
    // opportunity to wrap the line.
    // XXX This is different behavior from Blink.  Blink generates pairs of
    //     an NBSP and an ASCII white-space, but put NBSP at the end of the
    //     sequence.  We should follow the behavior for web-compat.
    if (maybeNBSPFollowingVisibleContent || !isPreviousCharASCIIWhiteSpace ||
        !followedByVisibleContentOrBRElement ||
        EditorUtils::IsContentPreformatted(
            *atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces
                 .GetContainerAsText())) {
      return NS_OK;
    }

    // Currently, we're at an NBSP following an ASCII space, and we need to
    // replace them with `"&nbsp; "` for avoiding collapsing white-spaces.
    MOZ_ASSERT(!atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces
                    .IsEndOfContainer());
    EditorDOMPointInText atFirstASCIIWhiteSpace =
        textFragmentData.GetFirstASCIIWhiteSpacePointCollapsedTo(
            atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces);
    AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
    uint32_t numberOfASCIIWhiteSpacesInStartNode =
        atFirstASCIIWhiteSpace.ContainerAsText() ==
                atPreviousCharOfEndOfVisibleWhiteSpaces.ContainerAsText()
            ? atPreviousCharOfEndOfVisibleWhiteSpaces.Offset() -
                  atFirstASCIIWhiteSpace.Offset()
            : atFirstASCIIWhiteSpace.ContainerAsText()->Length() -
                  atFirstASCIIWhiteSpace.Offset();
    // Replace all preceding ASCII white-spaces **and** the NBSP.
    uint32_t replaceLengthInStartNode =
        numberOfASCIIWhiteSpacesInStartNode +
        (atFirstASCIIWhiteSpace.ContainerAsText() ==
                 atPreviousCharOfEndOfVisibleWhiteSpaces.ContainerAsText()
             ? 1
             : 0);
    nsresult rv = aHTMLEditor.ReplaceTextWithTransaction(
        MOZ_KnownLive(*atFirstASCIIWhiteSpace.ContainerAsText()),
        atFirstASCIIWhiteSpace.Offset(), replaceLengthInStartNode,
        u"\x00A0 "_ns);
    if (NS_FAILED(rv)) {
      NS_WARNING("HTMLEditor::ReplaceTextWithTransaction() failed");
      return rv;
    }

    if (atFirstASCIIWhiteSpace.GetContainer() ==
        atPreviousCharOfEndOfVisibleWhiteSpaces.GetContainer()) {
      return NS_OK;
    }

    // We need to remove the following unnecessary ASCII white-spaces and
    // NBSP at atPreviousCharOfEndOfVisibleWhiteSpaces because we collapsed them
    // into the start node.
    rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
        EditorDOMPointInText::AtEndOf(
            *atFirstASCIIWhiteSpace.ContainerAsText()),
        atPreviousCharOfEndOfVisibleWhiteSpaces.NextPoint());
    NS_WARNING_ASSERTION(
        NS_SUCCEEDED(rv),
        "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed");
    return rv;
  }

  // XXX This is called when top-level edit sub-action handling ends for
  //     3 points at most.  However, this is not compatible with Blink.
  //     Blink touches white-space sequence which includes new character
  //     or following white-space sequence of new <br> element or, if and
  //     only if deleting range is followed by white-space sequence (i.e.,
  //     not touched previous white-space sequence of deleting range).
  //     This should be done when we change to make each edit action
  //     handler directly normalize white-space sequence rather than
  //     OnEndHandlingTopLevelEditSucAction().

  // First, check if the last character is an NBSP.  Otherwise, we don't need
  // to do nothing here.
  const EditorDOMPoint& atEndOfVisibleWhiteSpaces = visibleWhiteSpaces.EndRef();
  EditorDOMPointInText atPreviousCharOfEndOfVisibleWhiteSpaces =
      textFragmentData.GetPreviousEditableCharPoint(atEndOfVisibleWhiteSpaces);
  if (!atPreviousCharOfEndOfVisibleWhiteSpaces.IsSet() ||
      atPreviousCharOfEndOfVisibleWhiteSpaces.IsEndOfContainer() ||
      !atPreviousCharOfEndOfVisibleWhiteSpaces.IsCharNBSP()) {
    return NS_OK;
  }

  // Next, consider the range to collapse ASCII white-spaces before there.
  EditorDOMPointInText startToDelete, endToDelete;

  EditorDOMPointInText atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces =
      textFragmentData.GetPreviousEditableCharPoint(
          atPreviousCharOfEndOfVisibleWhiteSpaces);
  // If there are some preceding ASCII white-spaces, we need to treat them
  // as one white-space.  I.e., we need to collapse them.
  if (atPreviousCharOfEndOfVisibleWhiteSpaces.IsCharNBSP() &&
      atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces.IsSet() &&
      atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces
          .IsCharASCIISpace()) {
    startToDelete = textFragmentData.GetFirstASCIIWhiteSpacePointCollapsedTo(
        atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces);
    endToDelete = atPreviousCharOfPreviousCharOfEndOfVisibleWhiteSpaces;
  }
  // Otherwise, we don't need to remove any white-spaces, but we may need
  // to normalize the white-space sequence containing the previous NBSP.
  else {
    startToDelete = endToDelete =
        atPreviousCharOfEndOfVisibleWhiteSpaces.NextPoint();
  }

  AutoTransactionsConserveSelection dontChangeMySelection(aHTMLEditor);
  nsresult rv = aHTMLEditor.DeleteTextAndNormalizeSurroundingWhiteSpaces(
      startToDelete, endToDelete);
  NS_WARNING_ASSERTION(
      NS_SUCCEEDED(rv),
      "HTMLEditor::DeleteTextAndNormalizeSurroundingWhiteSpaces() failed");
  return rv;
}

EditorDOMPointInText WSRunScanner::TextFragmentData::
    GetPreviousNBSPPointIfNeedToReplaceWithASCIIWhiteSpace(
        const EditorDOMPoint& aPointToInsert) const {
  MOZ_ASSERT(aPointToInsert.IsSetAndValid());
  MOZ_ASSERT(VisibleWhiteSpacesDataRef().IsInitialized());
  NS_ASSERTION(VisibleWhiteSpacesDataRef().ComparePoint(aPointToInsert) ==
                       PointPosition::MiddleOfFragment ||
                   VisibleWhiteSpacesDataRef().ComparePoint(aPointToInsert) ==
                       PointPosition::EndOfFragment,
               "Previous char of aPoint should be in the visible white-spaces");

  // Try to change an NBSP to a space, if possible, just to prevent NBSP
  // proliferation.  This routine is called when we are about to make this
  // point in the ws abut an inserted break or text, so we don't have to worry
  // about what is after it.  What is after it now will end up after the
  // inserted object.
  EditorDOMPointInText atPreviousChar =
      GetPreviousEditableCharPoint(aPointToInsert);
  if (!atPreviousChar.IsSet() || atPreviousChar.IsEndOfContainer() ||
      !atPreviousChar.IsCharNBSP() ||
      EditorUtils::IsContentPreformatted(*atPreviousChar.ContainerAsText())) {
    return EditorDOMPointInText();
  }

  EditorDOMPointInText atPreviousCharOfPreviousChar =
      GetPreviousEditableCharPoint(atPreviousChar);
  if (atPreviousCharOfPreviousChar.IsSet()) {
    // If the previous char is in different text node and it's preformatted,
    // we shouldn't touch it.
    if (atPreviousChar.ContainerAsText() !=
            atPreviousCharOfPreviousChar.ContainerAsText() &&
        EditorUtils::IsContentPreformatted(
            *atPreviousCharOfPreviousChar.ContainerAsText())) {
      return EditorDOMPointInText();
    }
    // If the previous char of the NBSP at previous position of aPointToInsert
    // is an ASCII white-space, we don't need to replace it with same character.
    if (!atPreviousCharOfPreviousChar.IsEndOfContainer() &&
        atPreviousCharOfPreviousChar.IsCharASCIISpace()) {
      return EditorDOMPointInText();
    }
    return atPreviousChar;
  }

  // If previous content of the NBSP is block boundary, we cannot replace the
  // NBSP with an ASCII white-space to keep it rendered.
  const VisibleWhiteSpacesData& visibleWhiteSpaces =
      VisibleWhiteSpacesDataRef();
  if (!visibleWhiteSpaces.StartsFromNormalText() &&
      !visibleWhiteSpaces.StartsFromSpecialContent()) {
    return EditorDOMPointInText();
  }
  return atPreviousChar;
}

EditorDOMPointInText WSRunScanner::TextFragmentData::
    GetInclusiveNextNBSPPointIfNeedToReplaceWithASCIIWhiteSpace(
        const EditorDOMPoint& aPointToInsert) const {
  MOZ_ASSERT(aPointToInsert.IsSetAndValid());
  MOZ_ASSERT(VisibleWhiteSpacesDataRef().IsInitialized());
  NS_ASSERTION(VisibleWhiteSpacesDataRef().ComparePoint(aPointToInsert) ==
                       PointPosition::StartOfFragment ||
                   VisibleWhiteSpacesDataRef().ComparePoint(aPointToInsert) ==
                       PointPosition::MiddleOfFragment,
               "Inclusive next char of aPointToInsert should be in the visible "
               "white-spaces");

  // Try to change an nbsp to a space, if possible, just to prevent nbsp
  // proliferation This routine is called when we are about to make this point
  // in the ws abut an inserted text, so we don't have to worry about what is
  // before it.  What is before it now will end up before the inserted text.
  EditorDOMPointInText atNextChar =
      GetInclusiveNextEditableCharPoint(aPointToInsert);
  if (!atNextChar.IsSet() || NS_WARN_IF(atNextChar.IsEndOfContainer()) ||
      !atNextChar.IsCharNBSP() ||
      EditorUtils::IsContentPreformatted(*atNextChar.ContainerAsText())) {
    return EditorDOMPointInText();
  }

  EditorDOMPointInText atNextCharOfNextCharOfNBSP =
      GetInclusiveNextEditableCharPoint(atNextChar.NextPoint());
  if (atNextCharOfNextCharOfNBSP.IsSet()) {
    // If the next char is in different text node and it's preformatted,
    // we shouldn't touch it.
    if (atNextChar.ContainerAsText() !=
            atNextCharOfNextCharOfNBSP.ContainerAsText() &&
        EditorUtils::IsContentPreformatted(
            *atNextCharOfNextCharOfNBSP.ContainerAsText())) {
      return EditorDOMPointInText();
    }
    // If following character of an NBSP is an ASCII white-space, we don't
    // need to replace it with same character.
    if (!atNextCharOfNextCharOfNBSP.IsEndOfContainer() &&
        atNextCharOfNextCharOfNBSP.IsCharASCIISpace()) {
      return EditorDOMPointInText();
    }
    return atNextChar;
  }

  // If the NBSP is last character in the hard line, we don't need to
  // replace it because it's required to render multiple white-spaces.
  const VisibleWhiteSpacesData& visibleWhiteSpaces =
      VisibleWhiteSpacesDataRef();
  if (!visibleWhiteSpaces.EndsByNormalText() &&
      !visibleWhiteSpaces.EndsBySpecialContent() &&
      !visibleWhiteSpaces.EndsByBRElement()) {
    return EditorDOMPointInText();
  }

  return atNextChar;
}

// static
nsresult WhiteSpaceVisibilityKeeper::DeleteInvisibleASCIIWhiteSpaces(
    HTMLEditor& aHTMLEditor, const EditorDOMPoint& aPoint) {
  MOZ_ASSERT(aPoint.IsSet());
  Element* editingHost = aHTMLEditor.GetActiveEditingHost();
  TextFragmentData textFragmentData(aPoint, editingHost);
  const EditorDOMRange& leadingWhiteSpaceRange =
      textFragmentData.InvisibleLeadingWhiteSpaceRangeRef();
  // XXX Getting trailing white-space range now must be wrong because
  //     mutation event listener may invalidate it.
  const EditorDOMRange& trailingWhiteSpaceRange =
      textFragmentData.InvisibleTrailingWhiteSpaceRangeRef();
  DebugOnly<bool> leadingWhiteSpacesDeleted = false;
  if (leadingWhiteSpaceRange.IsPositioned() &&
      !leadingWhiteSpaceRange.Collapsed()) {
    nsresult rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
        leadingWhiteSpaceRange.StartRef(), leadingWhiteSpaceRange.EndRef());
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed to "
          "delete leading white-spaces");
      return rv;
    }
    leadingWhiteSpacesDeleted = true;
  }
  if (trailingWhiteSpaceRange.IsPositioned() &&
      !trailingWhiteSpaceRange.Collapsed() &&
      leadingWhiteSpaceRange != trailingWhiteSpaceRange) {
    NS_ASSERTION(!leadingWhiteSpacesDeleted,
                 "We're trying to remove trailing white-spaces with maybe "
                 "outdated range");
    nsresult rv = aHTMLEditor.DeleteTextAndTextNodesWithTransaction(
        trailingWhiteSpaceRange.StartRef(), trailingWhiteSpaceRange.EndRef());
    if (NS_FAILED(rv)) {
      NS_WARNING(
          "HTMLEditor::DeleteTextAndTextNodesWithTransaction() failed to "
          "delete trailing white-spaces");
      return rv;
    }
  }
  return NS_OK;
}

}  // namespace mozilla
