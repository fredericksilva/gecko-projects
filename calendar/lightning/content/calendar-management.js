//
//  calendar-management.js
//


var ltnCalendarManagerObserver = {
    QueryInterface: function(aIID) {
        if (!aIID.equals(Components.interfaces.calICalendarManagerObserver) ||
            !aIID.equals(Components.interfaces.nsISupports)) {
            throw Components.results.NS_ERROR_NO_INTERFACE;
        }

        return this;
    },

    onCalendarRegistered: function(aCalendar) {
        document.getElementById("calendarTree").boxObject.invalidate();
    },

    onCalendarUnregistering: function(aCalendar) {
        document.getElementById("calendarTree").boxObject.invalidate();
    },

    onCalendarDeleting: function(aCalendar) {
        document.getElementById("calendarTree").boxObject.invalidate();
    },

    onCalendarPrefSet: function(aCalendar, aName, aValue) {
    },

    onCalendarPrefDeleting: function(aCalendar, aName) {
    }
};

var ltnCompositeCalendarObserver = {
    QueryInterface: function(aIID) {
        // I almost wish that calICompositeObserver did not inherit from calIObserver,
        // and that the composite calendar maintined its own observer list
        if (!aIID.equals(Components.interfaces.calIObserver) ||
            !aIID.equals(Components.interfaces.calICompositeObserver) ||
            !aIID.equals(Components.interfaces.nsISupports)) {
            throw Components.results.NS_ERROR_NO_INTERFACE;
        }

        return this;
    },

    // calICompositeObserver
    onCalendarAdded: function (aCalendar) {
        document.getElementById("calendarTree").boxObject.invalidate();
    },

    onCalendarRemoved: function (aCalendar) {
        document.getElementById("calendarTree").boxObject.invalidate();
    },

    onDefaultCalendarChanged: function (aNewDefaultCalendar) {
        // make the calendar bold in the tree
    },

    // calIObserver
    onStartBatch: function() { },
    onEndBatch: function() { },
    onLoad: function() { },
    onAddItem: function(aItem) { },
    onModifyItem: function(aNewItem, aOldItem) { },
    onDeleteItem: function(aDeletedItem) { },
    onAlarm: function(aAlarmItem) { },
    onError: function(aErrNo, aMessage) { }
};

var ltnCalendarViewController = {
    QueryInterface: function(aIID) {
        if (!aIID.equals(Components.interfaces.calICalendarViewController) ||
            !aIID.equals(Components.interfaces.nsISupports)) {
            throw Components.results.NS_ERROR_NO_INTERFACE;
        }

        return this;
    },

    createNewEvent: function (aCalendar, aStartTime, aEndTime) {
        dump ("+++ createNewEvent!!\n");
    },

    modifyEventTime: function (aCalendar, aEvent, aNewStartTime, aNewEndTime) {
        dump ("+++ modifyEvent!\n");
    }
};

var activeCompositeCalendar = null;
function getCompositeCalendar()
{
    if (activeCompositeCalendar == null) {
        activeCompositeCalendar =
            ltnCreateInstance("@mozilla.org/calendar/calendar;1?type=composite",
                              "calICompositeCalendar");
        activeCompositeCalendar.addObserver(ltnCompositeCalendarObserver, 0);
    }

    return activeCompositeCalendar;
}

var activeCalendarManager;
function getCalendarManager()
{
    if (!activeCalendarManager) {
        activeCalendarManager = ltnGetService("@mozilla.org/calendar/manager;1",
                                              "calICalendarManager");
        activeCalendarManager.addObserver(ltnCalendarManagerObserver);
    }

    return activeCalendarManager;
}

function getCalendars()
{
    return getCalendarManager().getCalendars({});
}

function ltnNewCalendar()
{
    openCalendarWizard(ltnSetTreeView);
}

var ltnCalendarTreeView = {
    get rowCount()
    {
        try {
            return getCalendars().length;
        } catch (e) {
            return 0;
        }
    },

    getCellProperties: function (row, col, properties)
    {
        if (col.id == "col-calendar-Checkbox") {
            var cal = getCalendars()[row];
            if (getCompositeCalendar().getCalendar(cal.uri)) {
                properties.AppendElement(ltnGetAtom("checked"));
            }
        }
    },

    cycleCell: function (row, col)
    {
        var cal = getCalendars()[row];
        if (getCompositeCalendar().getCalendar(cal.uri)) {
            // need to remove it
            getCompositeCalendar().removeCalendar(cal.uri);
            dump ("calendar removed\n");
        } else {
            // need to add it
            getCompositeCalendar().addCalendar(cal);
            dump ("calendar added\n");
        }
        document.getElementById("calendarTree").boxObject.invalidateRow(row);
    },

    getCellValue: function (row, col)
    {
        if (col.id == "col-calendar-Checkbox") {
            var cal = getCalendars()[row];
            if (getCompositeCalendar().getCalendar(cal.uri))
                return "true";
            return "false";
        }

        dump ("*** Bad getCellValue (row: " + row + " col id: " + col.id + ")\n");
        return null;
    },

    setCellValue: function (row, col, value)
    {
        if (col.id == "col-calendar-Checkbox") {
            var cal = getCellValue()[row];
            if (value == "true") {
                getCompositeCalendar().addCalendar(cal);
            } else {
                getCompositeCalendar().removeCalendar(cal.uri);
            }
            return;
        }

        dump ("*** Bad setCellText (row: " + row + " col id: " + col.id + " val: " + value + ")\n");
    },

    getCellText: function (row, col)
    {
        if (col.id == "col-calendar-Checkbox") {
            return "";          // tooltip
        }

        if (col.id == "col-calendar-Calendar") {
            try {
                return getCalendars()[row].name;
            } catch (e) {
                return "<Unknown " + row + ">";
            }
        }

        dump ("*** Bad getCellText (row: " + row + " col id: " + col.id + ")\n");
        return null;
    },

    isEditable: function(row, col) { return false; },
    setTree: function(treebox) { this.treebox = treebox; },
    isContainer: function(row) { return false; },
    isSeparator: function(row) { return false; },
    isSorted: function(row) { return false; },
    getLevel: function(row) { return 0; },
    getImageSrc: function(row, col) { return null; },
    getRowProperties: function(row, props) { },
    getColumnProperties: function(colid, col, props) { }
};

function ltnSetTreeView()
{
    document.getElementById("calendarTree").view = ltnCalendarTreeView;
}

window.addEventListener("load", ltnSetTreeView, false);
